/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA3600_SGX_BM_H
#define GMA3600_SGX_BM_H
#include "sgx545_abi.h"
/* Caller pins WC pages and holds the native DDK lock. EUCLEAN means cleanup
 * could not prove alias PTE retirement: retain the original RAM pin forever.
 * No BM/OS handle or borrowed scatter array escapes a successful call. */
int gma3600_sgx_bm_cycle(void *heap, const __u32 *physical, __u32 count,
	void *linear, __u32 bytes, __u32 flags, struct sgx545_bm_check *out);
struct gma_sgx_mmu_binding;
/* Retained BM ownership; backing stays pinned until release succeeds. A failed
 * constructor may return an owned quarantine object and binding on EUCLEAN. */
int gma3600_sgx_bm_allocate(void *heap, const __u32 *physical, __u32 count,
	void *linear, __u32 bytes, __u32 flags, __u32 alignment,
	void **owner, struct gma_sgx_mmu_binding *binding);
int gma3600_sgx_bm_verify(void *owner, const __u32 *physical, __u32 count, __u32 *tables);
int gma3600_sgx_bm_info(void *owner, void *native_memory, int attach);
int gma3600_sgx_bm_release(void **owner);
#endif
