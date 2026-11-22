// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include "memory_stubs.h"

static int arrays, pages_live, array_calls, page_calls, wc_calls, wb_calls;
static int fail_array, fail_page;
static bool high_page, fail_wc;

void *kvmalloc_array(size_t n, size_t size, unsigned int flags)
{
    (void)flags;
    if (++array_calls == fail_array) return NULL;
    arrays++;
    return calloc(n, size);
}
void kvfree(void *p) { if (p) { arrays--; free(p); } }
struct page *alloc_page(unsigned int flags)
{
    struct page *p;
    assert(flags & __GFP_DMA32);
    if (++page_calls == fail_page) return NULL;
    p = calloc(1, sizeof(*p));
    p->phys = high_page ? 0x100000000ULL : 0x100000ULL + page_calls * 4096;
    pages_live++;
    return p;
}
phys_addr_t page_to_phys(struct page *p) { return p->phys; }
void __free_page(struct page *p)
{
    assert(!p->wc); /* Never return WC pages to the general allocator. */
    pages_live--;
    free(p);
}
int set_pages_array_wc(struct page **pages, int n)
{
    int i;
    wc_calls++;
    for (i = 0; i < (fail_wc ? n / 2 : n); i++) pages[i]->wc = true;
    return fail_wc ? -EIO : 0;
}
int set_pages_array_wb(struct page **pages, int n)
{
    int i;
    wb_calls++;
    for (i = 0; i < n; i++) pages[i]->wc = false;
    return 0;
}
static void reset(void)
{
    assert(!arrays && !pages_live);
    array_calls = page_calls = wc_calls = wb_calls = 0;
    fail_array = fail_page = 0;
    high_page = fail_wc = false;
}
int main(void)
{
    struct gma3600_gem_object obj = { .num_pages = 4 };
    int i;
    reset();
    assert(!gma3600_gem_alloc_pages(&obj));
    assert(obj.pages_wc && wc_calls == 1 && pages_live == 4);
    for (i = 0; i < obj.num_pages; i++) {
        assert(obj.pages[i]->wc);
        assert(obj.phys_addrs[i] == obj.pages[i]->phys);
        assert(!(obj.phys_addrs[i] >> 32));
    }
    gma3600_gem_free_pages(&obj);
    assert(!obj.pages_wc && wb_calls == 1);
    assert(!obj.pages && !obj.phys_addrs);
    gma3600_gem_free_pages(&obj); /* Error unwind may reach cleanup twice. */
    assert(wb_calls == 1);

    for (i = 1; i <= 2; i++) {
        reset(); fail_array = i;
        assert(gma3600_gem_alloc_pages(&obj) == -ENOMEM);
        assert(!arrays && !pages_live && !obj.pages && !obj.phys_addrs);
    }
    for (i = 1; i <= 4; i++) {
        reset(); fail_page = i;
        assert(gma3600_gem_alloc_pages(&obj) == -ENOMEM);
        assert(!arrays && !pages_live && wc_calls == 0);
    }
    reset(); high_page = true;
    assert(gma3600_gem_alloc_pages(&obj) == -EOVERFLOW);
    assert(!arrays && !pages_live && wc_calls == 0);

    reset(); fail_wc = true;
    assert(gma3600_gem_alloc_pages(&obj) == -EIO);
    assert(!arrays && !pages_live && wb_calls == 1 && !obj.pages_wc);
    reset(); obj.num_pages = 0;
    assert(gma3600_gem_alloc_pages(&obj) == -EINVAL);
    assert(!arrays && !pages_live);
    puts("Memory regression tests passed: low physical pages, WC/WB lifetime, allocation/cache failure cleanup");
    return 0;
}
