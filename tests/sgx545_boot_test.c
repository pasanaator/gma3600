// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gma3600_sgx_boot.h"
#include "fixtures/sgx545_boot_capture.h"

struct hardware {
	struct gma_sgx_boot *boot;
	unsigned int acquired, verifies, writes, resets, published, polls, delays, contained, quiesces, released;
	unsigned int fail_write, never_complete, complete_after, dead, frozen_clock, late_completion, backwards;
	int acquire_error, verify_error, reset_error, publish_error, status_error, quiesce_error, release_error;
	__u64 clock, reset_duration;
	struct { unsigned int offset, value; } log[100];
};
static struct gma_sgx_boot boot;
static struct hardware hw;
static int acquire(void *context, const struct gma_sgx_boot_plan *plan)
{
	struct hardware *h = context;
	assert(!h->dead && plan->memory[9].address == 0xf2000000);
	h->acquired++; return h->acquire_error;
}
static int verify(void *context, const struct gma_sgx_boot_plan *plan)
{
	struct hardware *h = context;
	assert(!h->dead && h->acquired && plan->masked_interrupt_writes == 1);
	h->verifies++; return h->verify_error;
}
static int write_reg(void *context, __u32 offset, __u32 value)
{
	struct hardware *h = context;
	assert(!h->dead && h->boot->owned && h->writes < 100);
	if (offset == 0x130 || offset == 0x110) assert(!value);
	if (offset == GMA_SGX_BOOT_KICK2) assert(value == 1 && h->resets == 1 && h->published == 1);
	h->log[h->writes].offset = offset; h->log[h->writes].value = value;
	h->writes++;
	/* An error may follow a posted hardware write; the owner must survive. */
	return h->writes == h->fail_write ? -EIO : 0;
}
static int reset(void *context)
{
	struct hardware *h = context;
	assert(!h->dead && h->writes == 6 && h->boot->owned);
	h->resets++; h->clock += h->reset_duration;
	return h->reset_error;
}
static int publish(void *context)
{
	struct hardware *h = context;
	assert(!h->dead && h->writes == 44 && h->boot->owned);
	h->published++; return h->publish_error;
}
static int status(void *context, __u32 *value)
{
	struct hardware *h = context;
	assert(!h->dead && h->writes == 45 && h->published && h->boot->owned);
	h->polls++;
	*value = !h->never_complete && h->polls >= h->complete_after ? 1 : 0;
	if (h->late_completion) h->clock += GMA_SGX_BOOT_DEADLINE_US;
	return h->status_error;
}
static __u64 now(void *context)
{
	struct hardware *h = context;
	assert(!h->dead);
	if (h->backwards && h->writes) return h->clock - 1;
	return h->clock;
}
static void delay(void *context, __u32 usec)
{
	struct hardware *h = context;
	assert(!h->dead && usec == 50);
	if (!h->frozen_clock) h->clock += usec;
	h->delays++;
}
static int contain(void *context)
{
	struct hardware *h = context;
	assert(!h->dead && h->boot->owned && h->boot->touched);
	h->contained++; return 0;
}
static int quiesce(void *context)
{
	struct hardware *h = context;
	assert(!h->dead && h->boot->owned && h->contained);
	h->quiesces++; return h->quiesce_error;
}
static int release(void *context)
{
	struct hardware *h = context;
	assert(h->boot->owned && h->boot->quiescent && h->acquired);
	assert(!h->released); h->released++;
	return h->release_error;
}
static const struct gma_sgx_boot_ops ops = {
	.acquire=acquire, .verify=verify, .write=write_reg, .reset=reset, .publish=publish,
	.status=status, .now_us=now, .delay_us=delay, .contain=contain, .quiesce=quiesce, .release=release
};
static void setup(void)
{
	memset(&boot, 0, sizeof(boot)); memset(&hw, 0, sizeof(hw));
	hw.boot = &boot; hw.complete_after = 1; hw.quiesce_error = -EBUSY; hw.clock = 1000;
}
static int start(void)
{
	return gma_sgx_boot_start(&boot, &captured_info, captured_memory, 15, &ops, &hw);
}
static void plan_tests(void)
{
	struct gma_sgx_boot_plan plan;
	struct sgx545_init2_info32 info;
	struct gma_sgx_boot_memory memory[15];
	unsigned int i, phase, word;
	assert(!gma_sgx_boot_plan_build(&captured_info, captured_memory, 15, &plan));
	assert(!plan.info.scripts.part2[3].value && captured_info.scripts.part2[3].value == 0x4000);
	assert(plan.masked_interrupt_writes == 1 && plan.info.scripts.part2[33].value == captured_memory[2].address);
	/* A new VA must be reflected in the script. Alias substitution alone,
	 * even with valid metadata for the original allocation, is rejected. */
	info = captured_info; info.scripts.part2[33].value += 4096;
	assert(gma_sgx_boot_plan_build(&info, captured_memory, 15, &plan) == -EPROTO);
	for (phase = 0; phase < 3; phase++) {
		unsigned int count = phase == 0 ? 2 : phase == 1 ? 38 : 9;
		for (i = 0; i < count; i++) for (word = 0; word < 3; word++) {
			struct sgx545_init_command32 *commands;
			info = captured_info;
			commands = phase == 0 ? info.scripts.part1 : phase == 1 ? info.scripts.part2 : info.scripts.deinit;
			if (word == 0) commands[i].op ^= 3;
			if (word == 1) commands[i].offset ^= 4;
			if (word == 2) commands[i].value ^= 1;
			assert(gma_sgx_boot_plan_build(&info, captured_memory, 15, &plan) == -EPROTO);
		}
	}
	for (i = 0; i < 15; i++) for (word = 0; word < 4; word++) {
		memcpy(memory, captured_memory, sizeof(memory));
		if (word == 0) memory[i].token ^= 0x100;
		if (word == 1) memory[i].address++;
		if (word == 2) memory[i].bytes++;
		if (word == 3) memory[i].flags ^= 2;
		assert(gma_sgx_boot_plan_build(&captured_info, memory, 15, &plan));
	}
	info = captured_info; info.scripts.part1[2] = (struct sgx545_init_command32){2, 0xffffffff, 0xffffffff};
	info.scripts.part2[63] = (struct sgx545_init_command32){1, 0x130, 0xffffffff};
	assert(!gma_sgx_boot_plan_build(&info, captured_memory, 15, &plan));
	assert(!plan.info.scripts.part1[2].offset && !plan.info.scripts.part2[63].op);
	memcpy(memory, captured_memory, sizeof(memory)); memory[1].address = memory[0].address;
	assert(gma_sgx_boot_plan_build(&captured_info, memory, 15, &plan) == -EINVAL);
	memcpy(memory, captured_memory, sizeof(memory)); memory[9].address = 0xf3000000;
	assert(gma_sgx_boot_plan_build(&captured_info, memory, 15, &plan) == -ERANGE);
	memory[9].address = 0xfffff000;
	assert(gma_sgx_boot_plan_build(&captured_info, memory, 15, &plan) == -EPROTO);
	assert(gma_sgx_boot_plan_build(&captured_info, memory, 14, &plan) == -EINVAL);
	for (i = 0; i < sizeof(plan); i++) assert(!((unsigned char *)&plan)[i]);
	for (i = 0; i < 13; i++) {
		info = captured_info; info.struct_sizes[i] += 4;
		assert(gma_sgx_boot_plan_build(&info, captured_memory, 15, &plan) == -EPROTO);
	}
	for (i = 0; i < 11; i++) {
		info = captured_info; info.host_kick[i]++;
		assert(gma_sgx_boot_plan_build(&info, captured_memory, 15, &plan) == -EPROTO);
	}
}
int main(void)
{
	unsigned int i;
	plan_tests();
	setup();
	{
		struct sgx545_init2_info32 changed = captured_info;
		changed.scripts.part2[3].value = 0xffffffff;
		assert(gma_sgx_boot_start(&boot, &changed, captured_memory, 15, &ops, &hw) == -EPROTO);
		assert(boot.phase == GMA_SGX_BOOT_VALIDATE && !hw.acquired && !hw.writes && !boot.owned);
	}
	setup(); assert(!start() && boot.state == GMA_SGX_BOOT_READY && boot.owned && !hw.released);
	assert(boot.writes == 45 && boot.polls == 1 && boot.status == 1);
	assert(hw.log[0].offset == 0x130 && hw.log[1].offset == 0x110);
	assert(hw.log[6+3].offset == 0x130 && !hw.log[6+3].value);
	assert(start() == -EALREADY);
	assert(gma_sgx_boot_stop(&boot) == -EBUSY && boot.owned && !hw.released);
	hw.quiesce_error = 0;
	assert(!gma_sgx_boot_stop(&boot) && !boot.owned && hw.released == 1);
	assert(!gma_sgx_boot_stop(&boot) && hw.released == 1);
	/* Inject failure at every posted register write, both with and without
	 * proven quiescence. Not one failing write permits premature page release. */
	for (i = 1; i <= 45; i++) {
		setup(); hw.fail_write = i;
		assert(start() == -EIO && boot.touched && boot.owned && !hw.released && boot.state == GMA_SGX_BOOT_QUARANTINED);
		assert(hw.writes == i && hw.contained == 1 && hw.quiesces == 1);
		assert(start() == -EALREADY);
		setup(); hw.fail_write = i; hw.quiesce_error = 0;
		assert(start() == -EIO && !boot.owned && hw.released == 1 && boot.state == GMA_SGX_BOOT_STOPPED);
	}
	setup(); hw.acquire_error = -ENOMEM;
	assert(start() == -ENOMEM && !boot.owned && !hw.writes && !hw.released);
	setup(); hw.verify_error = -EUCLEAN;
	assert(start() == -EUCLEAN && !boot.owned && !hw.writes && hw.released == 1);
	setup(); hw.verify_error = -EUCLEAN; hw.release_error = -EIO;
	assert(start() == -EUCLEAN && boot.owned && hw.released == 1);
	assert(gma_sgx_boot_stop(&boot) == -EIO && hw.released == 1);
	setup(); hw.reset_error = -EIO;
	assert(start() == -EIO && hw.writes == 6 && boot.owned && !hw.published);
	setup(); hw.publish_error = -EIO;
	assert(start() == -EIO && hw.writes == 44 && boot.owned && !hw.polls);
	setup(); hw.status_error = -EINTR;
	assert(start() == -EINTR && boot.owned && !hw.released);
	setup(); hw.never_complete = 1;
	assert(start() == -ETIMEDOUT && hw.polls == 10000 && boot.owned && !hw.released);
	setup(); hw.frozen_clock = hw.never_complete = 1;
	assert(start() == -ETIMEDOUT && hw.polls == 10000 && boot.owned);
	setup(); hw.reset_duration = GMA_SGX_BOOT_DEADLINE_US;
	assert(start() == -ETIMEDOUT && hw.writes == 6 && !hw.published && boot.owned);
	setup(); hw.late_completion = 1;
	assert(start() == -ETIMEDOUT && boot.owned && hw.polls == 1);
	setup(); hw.backwards = 1;
	assert(start() == -ETIMEDOUT && hw.writes == 1 && boot.owned);
	setup(); hw.complete_after = 4;
	assert(!start() && hw.polls == 4 && hw.delays == 3 && boot.owned);
	/* Detach invalidates MMIO. Neither stop nor last-close may touch the dead
	 * provider or release a stage the device may still access. */
	hw.dead = 1; gma_sgx_boot_detach(&boot);
	assert(boot.owned && boot.state == GMA_SGX_BOOT_QUARANTINED);
	assert(gma_sgx_boot_stop(&boot) == -EBUSY && !hw.released);
	setup();
	{
		struct gma_sgx_boot_ops without_quiesce = ops;
		without_quiesce.quiesce = NULL;
		assert(!gma_sgx_boot_start(&boot, &captured_info, captured_memory, 15, &without_quiesce, &hw));
		assert(gma_sgx_boot_stop(&boot) == -EOPNOTSUPP && boot.owned && !hw.released);
	}
	puts("SGX first-boot controller: captured policy, masked IRQs, all 45 write failures, deadlines, retained ownership, quiescence and detach passed (simulated IO only)");
	return 0;
}
