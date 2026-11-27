/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA3600_SGX_MMU_H
#define GMA3600_SGX_MMU_H
#include "sgx545_abi.h"
/* Internal, synchronous offline test. Does not retain addresses or mappings. */
int gma3600_sgx_mmu_exercise(void *heap, const __u32 *physical, __u32 count,
	__u32 flags, struct sgx545_mmu_check *result);
/* Zero-initialize before first use. Serialize with the native DDK mutex.
 * Keep the backing pages pinned until unbind succeeds. A failed binding must
 * retain its pages and metadata; retrying a released RA reservation is unsafe.
 */
struct gma_sgx_mmu_binding {
	void *heap;
	__u32 address, bytes, pages;
	int live, failed;
	__u32 memory_flags;
};
int gma3600_sgx_mmu_bind(void *heap, const __u32 *physical, __u32 count,
	__u32 alignment, struct gma_sgx_mmu_binding *binding);
int gma3600_sgx_mmu_bind_flags(void *heap, const __u32 *physical, __u32 count,
	__u32 alignment, __u32 memory_flags, struct gma_sgx_mmu_binding *binding);
int gma3600_sgx_mmu_verify(struct gma_sgx_mmu_binding *binding, const __u32 *physical,
	__u32 count, __u32 *tables);
int gma3600_sgx_mmu_root_matches(void *heap, __u32 physical);
int gma3600_sgx_mmu_unbind(struct gma_sgx_mmu_binding *binding);
/* Inspect native BM-owned aliases without taking or freeing their VA lease. */
int gma3600_sgx_mmu_checkpoint(void *heap, __u32 *capacity, __u32 *tables);
int gma3600_sgx_mmu_retired(void *heap, __u32 address, __u32 bytes, __u32 capacity, __u32 *tables);
#endif
