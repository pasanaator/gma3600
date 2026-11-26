/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA3600_SGX_INIT_H
#define GMA3600_SGX_INIT_H
#include "sgx545_abi.h"
/* Include after the DDK headers. These wrappers are metadata snapshots only:
 * they have no native BM/resource-manager registration and MUST NOT be passed
 * to DevInitSGXPart2KM, native free, power, or command-submission functions.
 * The audited gma3600_sgx_native_cycle may borrow them for InitDevInfo's
 * CPU-only prefix, or hold a file-owned CPU binding lease until explicit/automatic unbind.
 * Neither path registers these descriptors with BM/ResMan or enables hardware.
 */
struct gma_sgx_init_memory {
	__u32 token, address, bytes, flags;
	void *linear;
};
struct gma_sgx_native_init {
	SGX_BRIDGE_INIT_INFO info;
	PVRSRV_KERNEL_MEM_INFO memory[SGX545_INIT_MAX_BUFFERS];
};
void gma_sgx_init_requirements(struct sgx545_init_requirements *out);
int gma_sgx_init_tokens(const struct sgx545_init2_in32 *wire, __u32 *tokens, __u32 *count);
int gma_sgx_init_translate(const struct sgx545_init2_in32 *wire,
	const struct gma_sgx_init_memory *memory, __u32 count, struct gma_sgx_native_init *out);
int gma_sgx_init_check(const struct gma_sgx_native_init *snapshot,
	const struct gma_sgx_init_memory *memory, __u32 count);
int gma3600_sgx_native_cycle(void *device_node, struct gma_sgx_native_init *snapshot,
	struct sgx545_native_cycle *out);
int gma3600_sgx_native_begin(void *device_node, struct gma_sgx_native_init *snapshot,
	void **runtime, struct sgx545_native_cycle *out);
int gma3600_sgx_native_query(void *runtime, struct sgx545_native_cycle *out);
int gma3600_sgx_native_end(void **runtime);
#endif
