// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include "sgx_memory_stubs.h"
#include "gma3600_sgx_memory.h"

static int arrays, pages_live, array_calls, page_calls, wc_calls, uc_calls, wb_calls;
static int fail_array, fail_page;
static bool high_page, fail_cache, fail_wb;

void *kvmalloc_array(size_t n, size_t size, unsigned int flags)
{
    assert(flags & __GFP_ZERO);
    if (++array_calls == fail_array) return NULL;
    arrays++;
    return calloc(n, size);
}
void kvfree(void *p) { if (p) { arrays--; free(p); } }
struct page *alloc_page(unsigned int flags)
{
    struct page *p;
    assert((flags & (__GFP_DMA32 | __GFP_ZERO)) == (__GFP_DMA32 | __GFP_ZERO));
    if (++page_calls == fail_page) return NULL;
    p = calloc(1, sizeof(*p));
    /* Deliberately noncontiguous; virtual adjacency does not imply physical. */
    p->phys = high_page ? 0x100000000ULL : 0x100000ULL + page_calls * 12288;
    pages_live++;
    return p;
}
phys_addr_t page_to_phys(struct page *p) { return p->phys; }
void __free_page(struct page *p)
{
    assert(!p->wc);
    pages_live--;
    free(p);
}
static int cache_change(struct page **pages, int n)
{
    for (int i = 0; i < (fail_cache ? n / 2 : n); i++) pages[i]->wc = true;
    return fail_cache ? -EIO : 0;
}
int set_pages_array_wc(struct page **pages, int n)
{ wc_calls++; return cache_change(pages, n); }
int set_pages_array_uc(struct page **pages, int n)
{ uc_calls++; return cache_change(pages, n); }
int set_pages_array_wb(struct page **pages, int n)
{
    wb_calls++;
    if (fail_wb) return -EIO;
    for (int i = 0; i < n; i++) pages[i]->wc = false;
    return 0;
}
static void reset(void)
{
    assert(!arrays && !pages_live && !gma3600_sgx_live_pages());
    array_calls = page_calls = wc_calls = uc_calls = wb_calls = 0;
    fail_array = fail_page = 0;
    high_page = fail_cache = fail_wb = false;
}

int main(void)
{
    struct gma3600_sgx_pages *allocation = NULL;
    for (int cache = GMA_SGX_WB; cache <= GMA_SGX_UC; cache++) {
        reset();
        assert(!gma3600_sgx_pages_alloc(4, cache, &allocation));
        assert(allocation->count == 4 && pages_live == 4);
        assert(gma3600_sgx_live_pages() == 4);
        assert(allocation->pages[1]->phys != allocation->pages[0]->phys + 4096);
        assert(wc_calls == (cache == GMA_SGX_WC));
        assert(uc_calls == (cache == GMA_SGX_UC));
        assert(!gma3600_sgx_pages_free(allocation));
        assert(wb_calls == (cache != GMA_SGX_WB));
    }
    for (int failure = 1; failure <= 2; failure++) {
        reset(); fail_array = failure;
        assert(gma3600_sgx_pages_alloc(4, GMA_SGX_WC, &allocation) == -ENOMEM);
        assert(!allocation);
    }
    for (int failure = 1; failure <= 4; failure++) {
        reset(); fail_page = failure;
        assert(gma3600_sgx_pages_alloc(4, GMA_SGX_WC, &allocation) == -ENOMEM);
        assert(!allocation && !wc_calls);
    }
    reset(); high_page = true;
    assert(gma3600_sgx_pages_alloc(4, GMA_SGX_WC, &allocation) == -EOVERFLOW);
    assert(!allocation && !wc_calls);
    for (int cache = GMA_SGX_WC; cache <= GMA_SGX_UC; cache++) {
        reset(); fail_cache = true;
        assert(gma3600_sgx_pages_alloc(4, cache, &allocation) == -EIO);
        assert(!allocation && wb_calls == 1);
    }
    reset();
    assert(!gma3600_sgx_pages_alloc(4, GMA_SGX_WC, &allocation));
    fail_wb = true;
    assert(gma3600_sgx_pages_free(allocation) == -EIO);
    assert(pages_live == 4 && arrays == 2 && gma3600_sgx_live_pages() == 4);
    fail_wb = false;
    assert(!gma3600_sgx_pages_free(allocation));

    reset(); fail_cache = fail_wb = true;
    assert(gma3600_sgx_pages_alloc(4, GMA_SGX_WC, &allocation) == -EIO);
    assert(allocation && pages_live == 4); /* Preserve ownership for retry. */
    fail_wb = false;
    assert(!gma3600_sgx_pages_free(allocation));
    reset();
    assert(gma3600_sgx_pages_alloc(0, GMA_SGX_WC, &allocation) == -EINVAL);
    assert(!allocation);
    assert(gma3600_sgx_pages_alloc(4, -1, &allocation) == -EINVAL);
    assert(gma3600_sgx_pages_alloc(4, GMA_SGX_WC, NULL) == -EINVAL);
    assert(!gma3600_sgx_pages_free(NULL));
    reset();
    puts("SGX memory tests passed: WB/WC/UC aliases, low zeroed pages, allocation faults, WB restoration failures");
    return 0;
}
