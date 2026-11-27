/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA_SGX_LIVE_NATIVE_H
#define GMA_SGX_LIVE_NATIVE_H
#include "sgx545_abi.h"
struct gma_sgx_live_roots {
	__u32 kernel_pd, reset_pd, edm_dir, bank, mmu36;
};
int gma3600_sgx_native_live_prepare(void *runtime, void *registers, struct gma_sgx_live_roots *roots);
int gma3600_sgx_native_live_arm(void *runtime);
int gma3600_sgx_native_live_reset(void *runtime, __u32 *pvr_error);
int gma3600_sgx_native_live_publish(void *runtime);
int gma3600_sgx_native_live_status(void *runtime, __u32 *status);
#endif
