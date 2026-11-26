/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA3600_SGX_BOOT_H
#define GMA3600_SGX_BOOT_H
#if !defined(__KERNEL__) && !defined(SGX545_FREESTANDING)
#define SGX545_FREESTANDING
#endif
#include "sgx545_abi.h"

/* Internal preparation for the first polling-only boot, not a userspace ABI.
 * This controller is NOT connected to a live MMIO provider or bridge ioctl.
 * The observed recipe includes registers absent from both public header sets;
 * matching it is a constrained input policy, not proof of hardware safety.
 */
#define GMA_SGX_BOOT_BUFFERS 15U
#define GMA_SGX_BOOT_DEADLINE_US 500000U
#define GMA_SGX_BOOT_POLL_US 50U
#define GMA_SGX_BOOT_KICK2 0x0ae8U
#define GMA_SGX_BOOT_INIT_COMPLETE 1U
struct gma_sgx_boot_memory { __u32 token, address, bytes, flags; };
struct gma_sgx_boot_plan {
	struct sgx545_init2_info32 info;
	struct gma_sgx_boot_memory memory[GMA_SGX_BOOT_BUFFERS];
	__u32 masked_interrupt_writes;
};
int gma_sgx_boot_plan_build(const struct sgx545_init2_info32 *info,
	const struct gma_sgx_boot_memory *memory, __u32 count, struct gma_sgx_boot_plan *out);

enum gma_sgx_boot_state {
	GMA_SGX_BOOT_NEW, GMA_SGX_BOOT_STARTING, GMA_SGX_BOOT_READY,
	GMA_SGX_BOOT_STOPPED, GMA_SGX_BOOT_QUARANTINED
};
enum gma_sgx_boot_phase {
	GMA_SGX_BOOT_VALIDATE, GMA_SGX_BOOT_ACQUIRE, GMA_SGX_BOOT_VERIFY,
	GMA_SGX_BOOT_CLOCKS, GMA_SGX_BOOT_PART1, GMA_SGX_BOOT_RESET,
	GMA_SGX_BOOT_PART2, GMA_SGX_BOOT_PUBLISH, GMA_SGX_BOOT_KICK,
	GMA_SGX_BOOT_POLL
};
struct gma_sgx_boot_ops {
	/* All hooks are internal, bounded and serialized by the caller. acquire
	 * must pin the entire stage, native binding, original PD/reset pages,
	 * modules and parent resources, and exclude suspend/competing operations.
	 * A failed acquire unwinds itself. verify proves the actual retained PTEs,
	 * metadata, empty CCB and immutable contents match the copied plan.
	 * Neither hook may write GPU registers, enable DMA or change GPU power.
	 * Require an already accessible D0 parent and establish valid native timing.
	 */
	int (*acquire)(void *context, const struct gma_sgx_boot_plan *plan);
	int (*verify)(void *context, const struct gma_sgx_boot_plan *plan);
	int (*write)(void *context, __u32 offset, __u32 value);
	int (*reset)(void *context); /* bounded native reset, installs original PD */
	int (*publish)(void *context); /* clear init status, hostctl/CCB setup, WC barrier */
	int (*status)(void *context, __u32 *status); /* read barrier; faults return an error */
	__u64 (*now_us)(void *context); /* monotonic, including hook execution */
	void (*delay_us)(void *context, __u32 usec);
	/* Mask host interrupts / assert SGX reset. This is NOT quiescence proof. */
	int (*contain)(void *context);
	/* Return 0 ONLY with platform evidence that no GPU access can reach any
	 * owned page. NULL/any error means keep every page/reference indefinitely.
	 * No generic soft-reset readback is accepted as such evidence here.
	 */
	int (*quiesce)(void *context);
	/* Called once after quiescence, or before any possible hardware access.
	 * Retire native references before backing. Failure retains the owner and
	 * must never be retried automatically after partial native destruction.
	 */
	int (*release)(void *context);
};
struct gma_sgx_boot {
	struct gma_sgx_boot_plan plan;
	const struct gma_sgx_boot_ops *ops;
	void *context;
	enum gma_sgx_boot_state state;
	enum gma_sgx_boot_phase phase;
	__u64 started_us;
	__u32 status, writes, polls;
	int error, stop_error, contain_error;
	unsigned int owned, touched, detached, quiescent, release_attempted;
};
/* Allocate zeroed, keep serialized, and never reinitialize a held owner.
 * The ops table and context must outlive every READY/QUARANTINED owner.
 * READY and QUARANTINED both retain ownership. Neither permits rendering.
 * A timeout, signal, copyout fault, last-close, unload or detach is never an
 * instruction to free this object while owned != 0.
 */
int gma_sgx_boot_start(struct gma_sgx_boot *boot, const struct sgx545_init2_info32 *info,
	const struct gma_sgx_boot_memory *memory, __u32 count,
	const struct gma_sgx_boot_ops *ops, void *context);
int gma_sgx_boot_stop(struct gma_sgx_boot *boot);
void gma_sgx_boot_detach(struct gma_sgx_boot *boot);
#endif
