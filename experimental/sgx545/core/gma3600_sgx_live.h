/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA_SGX_LIVE_H
#define GMA_SGX_LIVE_H
#include "gma3600_sgx_boot.h"
#include "gma3600_sgx_live_native.h"
struct task_struct;
struct gma_sgx_live_owner_ops {
	int (*acquire)(void *owner);
	int (*verify)(void *owner);
	int (*release)(void *owner);
};
struct gma_sgx_live {
	struct gma_sgx_boot boot;
	struct gma_sgx_live_roots roots;
	const struct gma_sgx_live_owner_ops *owner_ops;
	void *owner, *identity, *runtime, *registers;
	struct task_struct *task;
	unsigned int sealed, attached, native_accesses;
	int io_error;
	__u32 reset_error, bif_status, bif_fault, event_status, event_status2;
};
int gma_sgx_live_start(struct gma_sgx_live *live, const struct sgx545_init2_info32 *info,
	const struct gma_sgx_boot_memory *memory, unsigned int count,
	const struct gma_sgx_live_owner_ops *ops, void *owner, void *identity, void *runtime);
void gma_sgx_live_result(struct gma_sgx_live *live, struct sgx545_live_boot *out);
void gma_sgx_live_detach(struct gma_sgx_live *live);
/* Default native MMIO stays blocked. Only the owning task in a bounded reset
 * callback can consume these accesses, at the exact borrowed SGX base. */
int gma3600_sgx_live_read(void *base, unsigned int offset, unsigned int *value);
int gma3600_sgx_live_write(void *base, unsigned int offset, unsigned int value);
#endif
