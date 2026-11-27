/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA_SGX_LIVE_POLICY_H
#define GMA_SGX_LIVE_POLICY_H
#include "gma3600_sgx_live_native.h"
/* SGX545 public register offsets, compiler-checked in native source tests. */
#define GMA_LIVE_RESET 0x80U
#define GMA_LIVE_RESET_MASK 0x3fU
#define GMA_LIVE_BIF_STATUS 0xc04U
#define GMA_LIVE_BIF_FAULT 0xc08U
#define GMA_LIVE_EVENT_STATUS 0x12cU
#define GMA_LIVE_EVENT_STATUS2 0x118U
static inline int gma_sgx_reset_read_allowed(__u32 offset)
{
	return offset == GMA_LIVE_BIF_STATUS || offset == GMA_LIVE_BIF_FAULT ||
		offset == GMA_LIVE_EVENT_STATUS || offset == GMA_LIVE_EVENT_STATUS2 || offset == GMA_LIVE_RESET;
}
static inline int gma_sgx_reset_write_allowed(const struct gma_sgx_live_roots *r, __u32 offset, __u32 value)
{
	if (offset == GMA_LIVE_RESET) return value == 0 || value == 0x3e || value == 0x3f;
	if (offset == 0xc00) return !(value & ~0x1aU); /* pause/clear-fault/invalidate */
	if (offset == 0xccc) return value == r->mmu36;
	if (offset == 0xc74) return value == 0;
	if (offset == 0xc78) return value == 0 || value == r->bank;
	if (offset == 0xc84) return value == 0 || value == r->reset_pd ||
		(offset == r->edm_dir && value == r->kernel_pd);
	if (offset >= 0xc38 && offset <= 0xc70 && !(offset & 3))
		return value == 0 || (offset == r->edm_dir && value == r->kernel_pd);
	return offset == 0x134 || offset == 0x114; /* SGX-local event acknowledgement */
}
#endif
