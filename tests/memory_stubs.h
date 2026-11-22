/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA3600_MEMORY_TEST_STUBS_H
#define GMA3600_MEMORY_TEST_STUBS_H
#define __GMA3600_DRV_H__
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
typedef uint64_t phys_addr_t;
#define GFP_KERNEL 1U
#define __GFP_ZERO 2U
#define __GFP_DMA32 4U
#define upper_32_bits(value) ((uint32_t)((uint64_t)(value) >> 32))
#define WARN_ON_ONCE(value) ((void)(value))
struct page { phys_addr_t phys; bool wc; };
struct gma3600_gem_object {
    struct page **pages;
    phys_addr_t *phys_addrs;
    int num_pages;
    bool pages_wc;
};
void *kvmalloc_array(size_t n, size_t size, unsigned int flags);
void kvfree(void *p);
struct page *alloc_page(unsigned int flags);
phys_addr_t page_to_phys(struct page *p);
void __free_page(struct page *p);
int set_pages_array_wc(struct page **pages, int n);
int set_pages_array_wb(struct page **pages, int n);
int gma3600_gem_alloc_pages(struct gma3600_gem_object *obj);
void gma3600_gem_free_pages(struct gma3600_gem_object *obj);
#endif
