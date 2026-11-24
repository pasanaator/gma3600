/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA3600_SGX_BUFFERS_H
#define GMA3600_SGX_BUFFERS_H
#include <linux/types.h>
struct vm_area_struct;
struct gma_sgx_buffers;
struct sgx545_alloc_shared_out;
struct sgx545_map_info_out;
struct page;
struct gma_sgx_buffer_stats { u32 buffers, pages, maps, errors; };
struct gma_sgx_buffers *gma_sgx_buffers_create(void);
/* Stop invalidates file ownership, but existing VMAs keep their pages alive. */
void gma_sgx_buffers_stop(struct gma_sgx_buffers *pool);
int gma_sgx_buffer_alloc(struct gma_sgx_buffers *pool, void *owner, u32 token,
	u32 flags, u32 bytes, struct sgx545_alloc_shared_out *out);
int gma_sgx_buffer_free(struct gma_sgx_buffers *pool, void *owner, u32 token);
void gma_sgx_buffers_release_owner(struct gma_sgx_buffers *pool, void *owner);
int gma_sgx_buffer_map_info(struct gma_sgx_buffers *pool, void *owner, u32 token,
	struct sgx545_map_info_out *out);
int gma_sgx_buffer_mmap(struct gma_sgx_buffers *pool, void *owner, struct vm_area_struct *vma);
int gma_sgx_buffer_check(struct gma_sgx_buffers *pool, void *owner, u32 token, u32 expected, u32 fill);
/* Callback runs under the pool mutex; it must not retain the borrowed pages. */
int gma_sgx_buffer_visit(struct gma_sgx_buffers *pool, void *owner, u32 token,
	int (*visit)(void *, struct page **, u32), void *context);
int gma_sgx_buffer_pin(struct gma_sgx_buffers *pool, void *owner, u32 token, void **pin);
void gma_sgx_buffer_unpin(void *pin);
void gma_sgx_buffers_stats(struct gma_sgx_buffers *pool, struct gma_sgx_buffer_stats *out);
#endif
