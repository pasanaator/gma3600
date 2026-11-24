// SPDX-License-Identifier: GPL-2.0-only
/* Compiler-only metadata. No pointers, register access, or callable interface. */
#include "pvr_bridge.h"
#include "sgx_bridge.h"
#include "sgx_options.h"
#ifndef __KERNEL__
#define SGX545_FREESTANDING
#endif
#include "sgx545_abi.h"

#ifndef SGX_BUILD_OPTION_MASK
#define SGX_BUILD_OPTION_MASK (~0U)
#endif
#ifdef SGX545_ORIGINAL_INTEL
#define GMA_SGX_DEVICE_FREE_REPLY PVRSRV_BRIDGE_RETURN
#else
#define GMA_SGX_DEVICE_FREE_REPLY PVRSRV_BRIDGE_OUT_FREEDEVICEMEM
#endif
#define SGX545_ABI_VALUE(name, expression) (unsigned int)(expression),
static const unsigned int gma_sgx_init_abi[]
	__attribute__((used, section(".gma_sgx_abi"))) = {
#include "sgx545_init_abi_fields.inc"
};
#undef SGX545_ABI_VALUE
