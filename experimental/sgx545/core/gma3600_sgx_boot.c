// SPDX-License-Identifier: GPL-2.0-only
/* First-boot input policy and ownership controller. No live MMIO bindings. */
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif
#include "gma3600_sgx_boot.h"

static const struct sgx545_init_command32 part1[] = {
	{1, 0xa58, 0}, {1, 0xacc, 0}, {2, 0, 0}
};
/* Exact Intel 20120717 sequence observed on the Atom. Unnamed offsets are
 * deliberately kept numeric: public headers do not establish their meaning.
 * Three pointer/base values below are checked against ORIGINAL allocations,
 * never an alias. The host interrupt enable is normalized to zero only after
 * the original input sequence has passed validation.
 */
static const struct sgx545_init_command32 part2[] = {
	{1,0x818,0x7ff}, {1,0x810,1}, {1,0x814,0xffff}, {1,0x130,0x4000},
	{1,0x110,0}, {1,0x13c,0}, {1,0xa7c,0}, {1,0xa80,0},
	{1,0xa74,0x208200}, {1,0x4e8,0x100}, {1,0xa78,0x2c},
	{1,0xaac,0x45}, {1,0xabc,0xe}, {1,0x800,0x30}, {1,0x804,0xffff},
	{1,0xa00,0x7c000},
	{1,0xa0c,0x3f20000}, {1,0xa10,0x3f21000}, {1,0xa14,0x3f22000},
	{1,0xa18,0x3f23000}, {1,0xa1c,0x3f24000}, {1,0xa20,0x3f25000},
	{1,0xa24,0x3f26000}, {1,0xa28,0x3f27000}, {1,0xa2c,0x3f28000},
	{1,0xa30,0x3f29000}, {1,0xa34,0x1fc0000}, {1,0xa38,0x1fc1000},
	{1,0xa3c,0x1fc2000}, {1,0xa40,0x1fc3000}, {1,0xa48,0},
	{1,0xab8,0xf0000000}, {1,0xb30,0x100}, {1,0xac4,0},
	{1,0xa68,0}, {1,0xa6c,1}, {1,0xa70,6}, {1,0xa58,0x2204b00f},
	{2,0,0}
};
static const struct sgx545_init_command32 deinit[] = {
	{1,0xa58,0}, {1,0xa68,0}, {1,0xa6c,0}, {1,0xa70,0},
	{1,0xa48,0}, {1,0x130,0}, {1,0x110,0}, {1,0,0}, {1,4,0}, {2,0,0}
};
static const __u32 sizes[15] = {8192,8,4,68,236,84,25612,1568,8328,53096,24,56,24,256,104};
static const __u32 structs[13] = {688,480,1224,220,208,60,68,492,56,80,180,68,32};
static const __u32 kicks[11] = {0x11d,0x103,0,0xd4,0xeb,0x13a,0x173,0x156,0,0x157,0};
static const __u32 data[24] = {[8]=2, [10]=0x300, [11]=0x940, [12]=0xf0000000, [13]=0xf9000000};

static int script_matches(const struct sgx545_init_command32 *in,
	const struct sgx545_init_command32 *expected, unsigned int count,
	const struct gma_sgx_boot_memory *memory)
{
	unsigned int i;
	for (i = 0; i < count; i++) {
		__u32 value = expected[i].value;
		if (in[i].op != expected[i].op) return 0;
		if (in[i].op == 2) return 1; /* trailing words have no semantics */
		if (in[i].offset != expected[i].offset) return 0;
		if (expected == part2) {
			if (i == 30) value = (memory[9].address >> 7) | 0x06000000U;
			if (i == 33) value = memory[2].address;
			if (i == 34) value = memory[10].address;
		}
		if (in[i].value != value) return 0;
	}
	return 0;
}

int gma_sgx_boot_plan_build(const struct sgx545_init2_info32 *info,
	const struct gma_sgx_boot_memory *memory, __u32 count, struct gma_sgx_boot_plan *out)
{
	__u32 roles[9], i, j;
	if (!out) return -EINVAL;
	/* out must be distinct from the input. Clear on rejection; never leave a
	 * previous successful plan looking usable after failed validation. */
	memset(out, 0, sizeof(*out));
	if (!info || !memory || count != GMA_SGX_BOOT_BUFFERS) return -EINVAL;
	roles[0]=info->ccb; roles[1]=info->ccb_control; roles[2]=info->event_kicker;
	roles[3]=info->host_control; roles[4]=info->ta3d_control; roles[5]=info->misc;
	roles[6]=info->hwperf; roles[7]=info->ta_signature; roles[8]=info->signature_3d;
	for (i = 0; i < count; i++) {
		const struct gma_sgx_boot_memory *m = &memory[i];
		__u64 end = (__u64)m->address + ((m->bytes + 4095U) & ~4095U);
		__u32 flags = SGX545_HEAP_FLAGS | (i < 9 ? SGX545_INTEL_INIT_FLAGS : 9U);
		if (!m->token || m->bytes != sizes[i] || m->flags != flags || !m->address ||
		    (m->address & 4095U) || end > 0x100000000ULL ||
		    m->token != (i < 9 ? roles[i] : info->mem_handles[i - 9])) return -EPROTO;
		/* The pinned kernel code/data heaps, not a VA selected by a caller. */
		if (m->address < (i < 9 ? 0xf4000000U : 0xf2000000U) ||
		    end > (i < 9 ? 0xf8fff000ULL : 0xf207f000ULL)) return -ERANGE;
		for (j = 0; j < i; j++) {
			__u64 other_end = (__u64)memory[j].address + ((memory[j].bytes + 4095U) & ~4095U);
			if (m->token == memory[j].token ||
			    ((__u64)m->address < other_end && (__u64)memory[j].address < end)) return -EINVAL;
		}
	}
	for (i = 6; i < 18; i++) if (info->mem_handles[i]) return -EPROTO;
	if (info->build_options != 0x60218 || memcmp(info->struct_sizes, structs, sizeof(structs)) ||
	    memcmp(info->host_kick, kicks, sizeof(kicks)) || memcmp(info->dev_data, data, sizeof(data)) ||
	    info->edm_task[0] != 0xb14 || info->edm_task[1] != 0xb20 ||
	    info->clock_control[0] != 0xa6aa || info->clock_control[1] != 0xaaaaaa ||
	    info->clock_status_reg != 8 || info->clock_status_mask != 0xffffffdfU || info->cache_control != 2)
		return -EPROTO;
	if (!script_matches(info->scripts.part1, part1, sizeof(part1)/sizeof(part1[0]), memory) ||
	    !script_matches(info->scripts.part2, part2, sizeof(part2)/sizeof(part2[0]), memory) ||
	    !script_matches(info->scripts.deinit, deinit, sizeof(deinit)/sizeof(deinit[0]), memory)) return -EPROTO;
	memcpy(&out->info, info, sizeof(out->info));
	out->masked_interrupt_writes = 1;
	memcpy(out->memory, memory, sizeof(out->memory));
	/* Never carry unexamined trailing commands into a future executor. */
	memset(&out->info.scripts, 0, sizeof(out->info.scripts));
	memcpy(out->info.scripts.part1, part1, sizeof(part1));
	memcpy(out->info.scripts.part2, info->scripts.part2, sizeof(part2));
	memcpy(out->info.scripts.deinit, deinit, sizeof(deinit));
	out->info.scripts.part2[3].value = 0; /* EVENT_HOST_ENABLE, polling only */
	out->info.scripts.part2[38] = part2[38];
	return 0;
}

static int deadline(struct gma_sgx_boot *b)
{
	/* Unsigned subtraction also rejects a clock moving backwards. */
	return b->ops->now_us(b->context) - b->started_us >= GMA_SGX_BOOT_DEADLINE_US ? -ETIMEDOUT : 0;
}
static int write_one(struct gma_sgx_boot *b, __u32 offset, __u32 value)
{
	int error = deadline(b);
	if (error) return error;
	/* Mark before the call: a reported write error may still have side effects. */
	b->touched = 1; b->writes++;
	return b->ops->write(b->context, offset, value);
}
static int run_script(struct gma_sgx_boot *b, const struct sgx545_init_command32 *script, unsigned int count)
{
	unsigned int i;
	for (i = 0; i < count; i++) {
		int error;
		if (script[i].op == 2) return 0;
		if (script[i].op != 1) return -EPROTO;
		error = write_one(b, script[i].offset, script[i].value);
		if (error) return error;
	}
	return -EPROTO;
}
static int release_owner(struct gma_sgx_boot *b)
{
	if (b->release_attempted) return b->stop_error ? b->stop_error : -EUCLEAN;
	b->release_attempted = 1;
	b->stop_error = b->ops->release(b->context);
	if (b->stop_error) { b->state = GMA_SGX_BOOT_QUARANTINED; return b->stop_error; }
	b->owned = 0; b->state = GMA_SGX_BOOT_STOPPED;
	return 0;
}

int gma_sgx_boot_stop(struct gma_sgx_boot *b)
{
	if (!b) return -EINVAL;
	if (!b->owned) return 0;
	if (b->release_attempted) return b->stop_error;
	if (b->detached) return -EBUSY;
	if (!b->touched) { b->quiescent = 1; return release_owner(b); }
	b->contain_error = b->ops->contain ? b->ops->contain(b->context) : -EOPNOTSUPP;
	b->stop_error = b->ops->quiesce ? b->ops->quiesce(b->context) : -EOPNOTSUPP;
	if (b->stop_error) { b->state = GMA_SGX_BOOT_QUARANTINED; return b->stop_error; }
	b->quiescent = 1;
	return release_owner(b);
}

void gma_sgx_boot_detach(struct gma_sgx_boot *b)
{
	if (!b) return;
	b->detached = 1;
	if (!b->owned) return;
	if (!b->touched && !b->release_attempted) {
		b->quiescent = 1;
		release_owner(b);
	} else {
		/* MMIO may already be gone: never try containment or reset here. */
		b->state = GMA_SGX_BOOT_QUARANTINED;
		if (!b->stop_error) b->stop_error = -ENODEV;
	}
}

int gma_sgx_boot_start(struct gma_sgx_boot *b, const struct sgx545_init2_info32 *info,
	const struct gma_sgx_boot_memory *memory, __u32 count,
	const struct gma_sgx_boot_ops *ops, void *context)
{
	int error;
	if (!b || !ops || !ops->acquire || !ops->verify || !ops->write || !ops->reset ||
	    !ops->publish || !ops->status || !ops->now_us || !ops->delay_us || !ops->release) return -EINVAL;
	if (b->state != GMA_SGX_BOOT_NEW || b->owned || b->detached) return -EALREADY;
	b->state = GMA_SGX_BOOT_STARTING; b->phase = GMA_SGX_BOOT_VALIDATE;
	error = gma_sgx_boot_plan_build(info, memory, count, &b->plan);
	if (error) goto no_owner;
	b->ops = ops; b->context = context; b->phase = GMA_SGX_BOOT_ACQUIRE;
	error = ops->acquire(context, &b->plan);
	if (error) goto no_owner;
	b->owned = 1; b->phase = GMA_SGX_BOOT_VERIFY;
	error = ops->verify(context, &b->plan);
	if (error) goto failed;
	b->started_us = ops->now_us(context);
	b->phase = GMA_SGX_BOOT_CLOCKS;
	/* Mask both interrupt banks before any script. The plan masks the Intel
	 * script's later unmask too. The parent top-level IRQ remains disabled. */
	error = write_one(b, 0x130, 0);
	if (!error) error = write_one(b, 0x110, 0);
	if (!error) error = write_one(b, 0, b->plan.info.clock_control[0]);
	if (!error) error = write_one(b, 4, b->plan.info.clock_control[1]);
	if (error) goto failed;
	b->phase = GMA_SGX_BOOT_PART1;
	error = run_script(b, b->plan.info.scripts.part1, 64);
	if (error) goto failed;
	b->phase = GMA_SGX_BOOT_RESET;
	error = deadline(b);
	if (!error) error = ops->reset(context);
	if (error) goto failed;
	b->phase = GMA_SGX_BOOT_PART2;
	error = run_script(b, b->plan.info.scripts.part2, 64);
	if (error) goto failed;
	b->phase = GMA_SGX_BOOT_PUBLISH;
	error = deadline(b);
	if (!error) error = ops->publish(context);
	if (error) goto failed;
	b->phase = GMA_SGX_BOOT_KICK;
	error = write_one(b, GMA_SGX_BOOT_KICK2, 1);
	if (error) goto failed;
	b->phase = GMA_SGX_BOOT_POLL;
	for (b->polls = 0; b->polls < GMA_SGX_BOOT_DEADLINE_US / GMA_SGX_BOOT_POLL_US; ) {
		error = deadline(b);
		if (error) goto failed;
		b->polls++;
		error = ops->status(context, &b->status);
		if (error) goto failed;
		error = deadline(b);
		if (error) goto failed;
		if (b->status & GMA_SGX_BOOT_INIT_COMPLETE) { b->state = GMA_SGX_BOOT_READY; return 0; }
		ops->delay_us(context, GMA_SGX_BOOT_POLL_US);
	}
	error = -ETIMEDOUT; /* independent cap even with a broken/stalled clock */
failed:
	b->error = error;
	gma_sgx_boot_stop(b); /* Quarantine on missing/unproven quiescence. */
	return error;
no_owner:
	b->error = error; b->state = GMA_SGX_BOOT_STOPPED;
	return error;
}
