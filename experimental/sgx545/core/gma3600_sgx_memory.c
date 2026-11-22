// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atomic.h>
#include <linux/limits.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <asm/set_memory.h>
#include "gma3600_sgx_memory.h"

static atomic_long_t live_pages = ATOMIC_LONG_INIT(0);

unsigned long gma3600_sgx_live_pages(void)
{
	return atomic_long_read(&live_pages);
}

int gma3600_sgx_pages_free(struct gma3600_sgx_pages *allocation)
{
	unsigned int i;
	int ret;

	if (!allocation)
		return 0;
	if (allocation->cache_changed) {
		ret = set_pages_array_wb(allocation->pages, allocation->count);
		if (ret)
			return ret;
		allocation->cache_changed = false;
	}
	for (i = 0; i < allocation->count; i++) {
		if (allocation->pages[i]) {
			__free_page(allocation->pages[i]);
			atomic_long_dec(&live_pages);
		}
	}
	kvfree(allocation->pages);
	kvfree(allocation);
	return 0;
}

int gma3600_sgx_pages_alloc(unsigned int count, enum gma3600_sgx_cache cache,
			    struct gma3600_sgx_pages **result)
{
	struct gma3600_sgx_pages *allocation;
	unsigned int i;
	int ret, cleanup_ret;

	if (!result)
		return -EINVAL;
	*result = NULL;
	if (!count || count > INT_MAX || (unsigned int)cache > GMA_SGX_UC)
		return -EINVAL;
	allocation = kvmalloc_array(1, sizeof(*allocation), GFP_KERNEL | __GFP_ZERO);
	if (!allocation)
		return -ENOMEM;
	allocation->pages = kvmalloc_array(count, sizeof(*allocation->pages),
					 GFP_KERNEL | __GFP_ZERO);
	if (!allocation->pages) {
		kvfree(allocation);
		return -ENOMEM;
	}
	allocation->count = count;
	for (i = 0; i < count; i++) {
		allocation->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO | __GFP_DMA32);
		if (!allocation->pages[i]) {
			ret = -ENOMEM;
			goto fail;
		}
		atomic_long_inc(&live_pages);
		if (upper_32_bits(page_to_phys(allocation->pages[i]))) {
			ret = -EOVERFLOW;
			goto fail;
		}
	}
	if (cache != GMA_SGX_WB) {
		/* Cache changes may fail after touching part of the range. */
		allocation->cache_changed = true;
		ret = cache == GMA_SGX_WC ? set_pages_array_wc(allocation->pages, count) :
					 set_pages_array_uc(allocation->pages, count);
		if (ret)
			goto fail;
	}
	*result = allocation;
	return 0;
fail:
	cleanup_ret = gma3600_sgx_pages_free(allocation);
	if (cleanup_ret) {
		/* Return ownership so the caller can retry restoring WB. */
		*result = allocation;
		return cleanup_ret;
	}
	return ret;
}
