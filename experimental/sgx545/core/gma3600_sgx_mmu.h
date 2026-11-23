/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA3600_SGX_MMU_H
#define GMA3600_SGX_MMU_H
#include "sgx545_abi.h"
/* Internal, synchronous offline test. Does not retain addresses or mappings. */
int gma3600_sgx_mmu_exercise(void *heap, const __u32 *physical, __u32 count,
	__u32 flags, struct sgx545_mmu_check *result);
#endif
