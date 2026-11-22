/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA3600_SGX_MEMORY_H
#define GMA3600_SGX_MEMORY_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#endif

enum gma3600_sgx_cache { GMA_SGX_WB, GMA_SGX_WC, GMA_SGX_UC };
struct page;
struct gma3600_sgx_pages {
	struct page **pages;
	unsigned int count;
	bool cache_changed;
};

int gma3600_sgx_pages_alloc(unsigned int count, enum gma3600_sgx_cache cache,
			    struct gma3600_sgx_pages **result);
/* Caller removes vmap/mmap/device aliases first. On error the allocation is
 * retained, not returned to the general allocator with unsafe cache aliases.
 */
int gma3600_sgx_pages_free(struct gma3600_sgx_pages *allocation);
unsigned long gma3600_sgx_live_pages(void);
#endif
