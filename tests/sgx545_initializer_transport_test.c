// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#define SGX545_TRANSPORT_TEST
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "sgx545_abi.h"
static unsigned opens, closes, ioctls;
static int fail_open, fail_close, fail_ioctl;
static unsigned fail_command, fail_operation, query_calls, abort_calls;
static int corrupt_query;
static int corrupt_content;
static unsigned content_calls;
static unsigned native_calls;
static int corrupt_native;
static int runtime_fail_op = -1, runtime_corrupt_op = -1, runtime_live;
static int bm_fail_index = -1, bm_corrupt_index = -1, runtime_fail_requery;
static unsigned runtime_queries;
static int owned_fail_index = -1, owned_corrupt_index = -1;
static unsigned sequence[80], sequence_count;
static int owner_fail_op=-1, owner_malformed_op=-1, owner_held;
static unsigned owner_calls[3];
static unsigned live_mode, live_calls[2];
static int transport_open(const char *path, int flags)
{
	assert(!strcmp(path, "/dev/gma3600-sgx-bridge") && flags == (O_RDWR | O_CLOEXEC));
	opens++;
	if (fail_open) { errno = EACCES; return -1; }
	return (int)opens;
}
static int transport_close(int fd)
{
	assert(fd > 0); closes++;
	if (fail_close) { errno = EINTR; return -1; }
	return 0;
}
static int transport_ioctl(int fd, unsigned long command, void *arg)
{
	assert(fd > 0 && arg); ioctls++;
	if (fail_ioctl) { errno = EOPNOTSUPP; return -1; }
	if (command == SGX545_IOCTL_LIVE_BOOT) {
		struct sgx545_live_boot *r=arg;
		assert(runtime_live && r->services==10 && r->stage==200 && r->device==20 && r->operation<=1);
		unsigned op=r->operation;
		live_calls[op]++;
		assert(!r->state && !r->sealed && live_calls[0]==1);
		if ((live_mode==25 || live_mode==26) && !op) { errno=EFAULT; return -1; }
		if (live_mode==26 && op) { errno=EIO; return -1; }
		r->state=live_mode==24 ? 4 : 2; r->sealed=1; r->init_status=live_mode==24 ? 0 : 1;
		r->error=live_mode==24 ? ETIMEDOUT : 0;
		if (live_mode==27 && !op) r->reserved=1;
	} else if (command == SGX545_IOCTL_BOOT_OWNER) {
		struct sgx545_boot_owner *owner=arg;
		unsigned op=owner->operation;
		assert(runtime_live && owner->services==10 && owner->stage==200 && owner->device==20 && op<=2);
		assert(!owner->held && !owner->parent_abi && sequence_count<80);
		sequence[sequence_count++]=command; owner_calls[op]++;
		if ((int)op==owner_fail_op) { errno=EIO; return -1; }
		if (op==SGX545_OWNER_ACQUIRE) { assert(!owner_held); owner_held=1; }
		else assert(owner_held);
		if (op==SGX545_OWNER_RELEASE) owner_held=0;
		else { owner->held=owner->verified=1; owner->parent_abi=3; owner->buffers=owner->pages=11; }
		if ((int)op==owner_malformed_op) owner->held^=1;
	} else if (command == SGX545_IOCTL_BM_INFO) {
		struct sgx545_bm_info *info = arg;
		assert(runtime_live && info->services == 10 && info->stage == 200 && info->index < 11 && !info->owned);
		assert(sequence_count < 80); sequence[sequence_count++] = command;
		if ((int)info->index == owned_fail_index) { errno = EIO; return -1; }
		info->token = 0x100 + info->index; info->address = 0xf2000000 + info->index * 4096;
		info->bytes = 4096; info->pages = 1; info->memory_flags = SGX545_INTEL_INIT_FLAGS | SGX545_HEAP_FLAGS;
		info->alignment = 4096; info->owned = info->registered = 1;
		info->attached = (int)info->index == owned_corrupt_index ? 0 : 1;
	} else if (command == SGX545_IOCTL_BM_CHECK) {
		struct sgx545_bm_check *check = arg;
		assert(runtime_live && check->services == 10 && check->stage == 200 && check->index < 11 && !check->passes);
		assert(sequence_count < 80); sequence[sequence_count++] = command;
		if ((int)check->index == bm_fail_index) { errno = EIO; return -1; }
		check->token = 0x100 + check->index;
		check->original_address = 0xf2000000 + check->index * 4096;
		check->alias_address = 0xf3000000 + check->index * 4096;
		check->bytes = 4096; check->pages = 1;
		check->memory_flags = SGX545_INTEL_INIT_FLAGS | SGX545_HEAP_FLAGS;
		check->passes = (int)check->index == bm_corrupt_index ? 1 : 2;
		check->pt_before = check->pt_after = 1;
	} else if (command == SGX545_IOCTL_NATIVE_RUNTIME) {
		struct sgx545_native_runtime *request = arg;
		struct sgx545_native_cycle *state = &request->state;
		unsigned op = request->operation;
		assert(state->services == 10 && state->stage == 200 && state->device == 20 && !state->flags && !request->reserved);
		assert(op <= SGX545_NATIVE_UNBIND && sequence_count < 80); sequence[sequence_count++] = command;
		if (op == SGX545_NATIVE_QUERY) runtime_queries++;
		if ((runtime_fail_requery && runtime_queries == 2 && op == SGX545_NATIVE_QUERY) || (int)op == runtime_fail_op) { errno = EIO; return -1; }
		if (op == SGX545_NATIVE_BIND) { assert(!runtime_live); runtime_live = 1; }
		else assert(runtime_live);
		state->buffers = 11; state->pages = 11;
		if (op == SGX545_NATIVE_UNBIND) { runtime_live = 0; state->restored = (int)op == runtime_corrupt_op ? 0 : 1; }
		else {
			state->bound = 1; state->flags = SGX545_NATIVE_CCB_ROUNDTRIP; state->ccb_bytes = 8192; state->slots = 256;
			if ((int)op == runtime_corrupt_op) state->slots--;
		}
	} else if (command == SGX545_IOCTL_NATIVE_CYCLE) {
		struct sgx545_native_cycle *native = arg;
		assert(native->services == 10 && native->stage == 200 && native->device == 20 && !native->flags);
		assert(sequence_count < 80); sequence[sequence_count++] = command; native_calls++;
		if (command == fail_command) { errno = EIO; return -1; }
		native->bound = 1; native->restored = corrupt_native ? 0 : 1;
		native->flags = SGX545_NATIVE_CCB_ROUNDTRIP; native->buffers = 11; native->pages = 11;
		native->ccb_bytes = 8192; native->slots = 256;
	} else if (command == SGX545_IOCTL_INIT_CONTENT) {
		struct sgx545_init_content *content = arg;
		assert(content->services == 10 && content->stage == 200 && content->index < 11 && !content->flags);
		assert(sequence_count < 80); sequence[sequence_count++] = command; content_calls++;
		if (command == fail_command && content->index == 3) { errno = EIO; return -1; }
		content->token = 0x100 + content->index;
		content->bytes = 4096; content->address = 0xf2000000 + content->index * 4096;
		content->memory_flags = SGX545_INTEL_INIT_FLAGS | SGX545_HEAP_FLAGS;
		content->baseline_hash = content->current_hash = 0x12340000 + content->index;
		content->nonzero_bytes = 4000;
		if (corrupt_content == 1 && content->index == 5) { content->current_hash++; content->flags = SGX545_CONTENT_CHANGED; }
		if (corrupt_content == 2 && content->index == 4) content->token++;
	} else if (command == SGX545_IOCTL_INIT_CONTROL) {
		struct sgx545_init_control *control = arg;
		assert(control->services == 10 && control->stage == 200 && !control->flags);
		assert(sequence_count < 80); sequence[sequence_count++] = command + control->operation;
		if (control->operation) { abort_calls++; runtime_live = 0; } else query_calls++;
		if (command == fail_command && control->operation == fail_operation) { errno = EIO; return -1; }
		control->buffers = control->operation ? 0 : 11;
		control->pages = control->operation ? 0 : 11;
		if (corrupt_query && !control->operation) control->stage++;
	} else if (command == SGX545_IOCTL_INIT_STAGE || command == SGX545_IOCTL_INIT_VALIDATE) {
		struct sgx545_init_stage *stage = arg;
		assert(stage->services == 10 && stage->device == 20 && stage->request && stage->request_bytes == 2068);
		if (sizeof(void *) == 4) {
			const struct sgx545_init2_in32 *copy = (void *)(unsigned long)stage->request;
			assert(!copy->flags && copy->device == 20 && copy->info.build_options == 0x60218);
		}
		assert(!stage->stage && !stage->buffers && !stage->pages && !stage->flags);
		assert(sequence_count < 80); sequence[sequence_count++] = command;
		if (command == fail_command) { errno = ENOSPC; return -1; }
		stage->buffers = 11; stage->pages = 11;
		if (command == SGX545_IOCTL_INIT_STAGE) stage->stage = 200;
	} else if (command == SGX545_IOCTL_INIT_REQUIREMENTS) {
		struct sgx545_init_requirements *caps = arg;
		assert(caps->services == 10 && caps->device == 20);
		assert(sequence_count < 80); sequence[sequence_count++] = command;
		if (command == fail_command) { errno = EACCES; return -1; }
		caps->build_options = 0x360218; caps->build_option_mask = 0xffefffff;
		caps->struct_sizes[11] = 68; caps->struct_sizes[12] = 32;
	} else assert(command == SGX545_IOCTL_SERVICES);
	return 0;
}
#include "../tools/sgx545_initializer_transport.c"

static void handoff_tests(unsigned *input)
{
	struct sgx545_init2_in32 *request = (void *)input;
	unsigned *output = input + 800;
	struct sgx545_bridge32 wire = {SGX545_COMMAND(99), 28, (unsigned)(unsigned long)input,
		2068, (unsigned)(unsigned long)output, 8, 10};
	unsigned mode, before;
	for (mode = 0; mode < 28; mode++) {
		int blocker = mode == 0 ? drmOpen("psb", NULL) : -1;
		int fd = drmOpen("psb", NULL);
		memset(request, 0, sizeof(*request)); request->device = 20; request->flags = 0xdeadbeef;
		request->info.build_options = 0x60218;
		request->info.ccb = 0x100; request->info.ccb_control = 0x101; request->info.event_kicker = 0x102;
		request->info.host_control = 0x103; request->info.ta3d_control = 0x104; request->info.misc = 0x105;
		request->info.hwperf = 0x106; request->info.ta_signature = 0x107; request->info.signature_3d = 0x108;
		request->info.mem_handles[0] = 0x109; request->info.mem_handles[17] = 0x10a;
		request->info.scripts.part1[0] = (struct sgx545_init_command32){1, 0x100, 0x12345678};
		request->info.scripts.part1[1].op = 2;
		request->info.scripts.part2[0].op = request->info.scripts.deinit[0].op = 2;
		output[0] = output[1] = 0xfefefefe;
		query_calls = abort_calls = sequence_count = content_calls = native_calls = 0;
		fail_command = mode == 1 ? SGX545_IOCTL_INIT_REQUIREMENTS : mode == 2 ? SGX545_IOCTL_INIT_VALIDATE :
			mode == 3 ? SGX545_IOCTL_INIT_STAGE : mode == 4 || mode == 5 ? SGX545_IOCTL_INIT_CONTROL : mode == 7 ? SGX545_IOCTL_INIT_CONTENT : mode == 10 ? SGX545_IOCTL_NATIVE_CYCLE : 0;
		fail_operation = mode == 5 ? SGX545_STAGE_ABORT : SGX545_STAGE_QUERY;
		corrupt_query = mode == 6;
		corrupt_content = mode == 8 ? 1 : mode == 9 ? 2 : 0;
		corrupt_native = mode == 11;
		runtime_fail_op = mode >= 12 && mode <= 14 ? (int)mode - 12 : -1;
		runtime_corrupt_op = mode >= 15 && mode <= 17 ? (int)mode - 15 : -1;
		runtime_queries = 0; bm_fail_index = mode == 18 ? 3 : -1; bm_corrupt_index = mode == 19 ? 5 : -1; runtime_fail_requery = mode == 20;
		owned_fail_index = mode == 21 ? 3 : -1; owned_corrupt_index = mode == 22 ? 5 : -1;
		wire.bridge_id = SGX545_COMMAND(99); wire.in_size = 2068;
		if (mode>=23) {
			live_mode=mode; memset(live_calls,0,sizeof(live_calls));
			assert(!setenv("GMA3600_SGX_FIRMWARE_BOOT","1",1));
			int result=drmCommandWrite(fd,0x12,&wire,28);
			assert(result==(mode==24 ? -ETIMEDOUT : mode==26 ? -EFAULT : -EOPNOTSUPP));
			assert(live_calls[0]==1 && live_calls[1]==(mode>=25));
			assert(runtime_live && !abort_calls && !owner_held);
			assert(sequence[sequence_count-1]==SGX545_IOCTL_BOOT_OWNER);
			assert(output[0]==0xfefefefe && output[1]==0xfefefefe);
			assert(!drmClose(fd)); runtime_live=0;
			assert(!unsetenv("GMA3600_SGX_FIRMWARE_BOOT"));
			continue;
		}
		assert(drmCommandWrite(fd, 0x12, &wire, 28) == (mode == 1 ? -EACCES : mode == 2 || mode == 3 ? -ENOSPC :
			mode == 6 || mode == 9 || mode == 11 || (mode >= 15 && mode <= 17) || mode == 19 || mode == 22 ? -EPROTO : mode == 8 ? -EUCLEAN : mode >= 4 ? -EIO : -EOPNOTSUPP));
		assert(request->flags == 0xdeadbeef); /* original reserved word untouched */
		assert(output[0] == 0xfefefefe && output[1] == 0xfefefefe); /* never claim legacy success */
		assert(sequence_count == (mode == 1 ? 1 : mode == 2 ? 2 : mode == 3 ? 3 :
			mode == 7 ? 36 : mode == 8 ? 38 : mode == 9 ? 37 : mode == 10 || mode == 11 ? 6 :
			mode == 21 ? 13 : mode == 22 ? 15 : mode == 18 ? 24 : mode == 19 ? 26 : mode == 20 ? 32 : mode == 12 ? 7 : mode == 13 || mode == 16 ? 9 : mode == 15 ? 8 : mode == 14 || mode == 17 || mode == 0 || mode == 5 ? 46 : 5));
		assert(sequence[0] == SGX545_IOCTL_INIT_REQUIREMENTS);
		if (sequence_count > 1) assert(sequence[1] == SGX545_IOCTL_INIT_VALIDATE);
		if (sequence_count > 2) assert(sequence[2] == SGX545_IOCTL_INIT_STAGE);
		assert(query_calls == (mode == 0 || mode >= 4));
		assert(abort_calls == query_calls); /* cleanup even after a failed query */
		assert(content_calls == (mode == 0 || mode == 5 || mode == 14 || mode == 17 ? 11 : mode == 7 ? 4 : mode == 8 ? 6 : mode == 9 ? 5 : 0));
		assert(native_calls == (mode == 0 || mode == 5 || mode >= 7));
		assert(!runtime_live);
		assert(!owner_held);
		fail_command = 0; corrupt_query = corrupt_content = corrupt_native = 0;
		runtime_fail_op = runtime_corrupt_op = bm_fail_index = bm_corrupt_index = -1; runtime_fail_requery = 0; owned_fail_index = owned_corrupt_index = -1;
		if (blocker > 0) assert(!drmClose(blocker)); /* move the staged fd's bookkeeping slot */
		before = ioctls;
		wire.bridge_id = SGX545_COMMAND(68); wire.in_size = 8; input[1] = 0;
		assert(!drmCommandWrite(fd, 0x12, &wire, 28));
		assert(ioctls == before + (mode >= 1 && mode <= 3)); /* only delivered STAGE already aborted */
		input[1] = 1;
		assert(drmCommandWrite(fd, 0x12, &wire, 28) == -EOPNOTSUPP);
		wire.bridge_id = SGX545_COMMAND(99); wire.in_size = 2068;
		if (mode == 0 || mode >= 4) assert(drmCommandWrite(fd, 0x12, &wire, 28) == -EALREADY);
		if (mode == 0) {
			wire.bridge_id = SGX545_COMMAND(13); wire.in_size = 0;
			assert(!drmCommandWrite(fd, 0x12, &wire, 28));
			before = ioctls;
			wire.bridge_id = SGX545_COMMAND(68); wire.in_size = 8; input[1] = 0;
			assert(!drmCommandWrite(fd, 0x12, &wire, 28) && ioctls == before + 1);
		}
		assert(!drmClose(fd));
	}
}

int main(void)
{
	assert(!unsetenv("GMA3600_SGX_FIRMWARE_BOOT"));
	struct sgx545_bridge32 wire = {SGX545_COMMAND(0), 28, 0, 0, 0, 0, 1};
	unsigned before, i;
	int fd, other;
	unsigned *input = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS
#if __SIZEOF_POINTER__ == 8
		| MAP_32BIT
#endif
		, -1, 0);
	assert(input != MAP_FAILED && (unsigned long)input <= 0xfffff000UL);
	assert(drmOpen("i915", NULL) == -EINVAL && !opens);
	assert(drmOpen("psb", "pci:0:2:0") == -EINVAL && !opens);
	assert(drmOpen(NULL, NULL) == -EINVAL && !opens);
	assert(drmOpen(NULL, "pci:0000:00:03.0") == -EINVAL && !opens);
	assert(drmOpen(NULL, "pci:0001:00:02.0") == -EINVAL && !opens);
	assert(drmOpen(NULL, "pci:0000:00:02.1") == -EINVAL && !opens);
	assert(drmOpen("i915", "pci:0000:00:02.0") == -EINVAL && !opens);
	/* Reproduce the observed Intel selector, then its master check and first
	 * bridge request on the returned file. This failed before the PCI fix. */
	fd = drmOpen(NULL, "pci:0000:00:02.0");
	assert(fd > 0 && !drmCommandNone(fd, 21));
	assert(!drmCommandWrite(fd, 0x12, &wire, 28) && ioctls == 1);
	assert(!drmClose(fd) && !descriptor_count);
	ioctls = 0;
	fd = drmOpen("psb", "pci:0000:00:02.0");
	assert(fd > 0 && !drmClose(fd));
	fail_open = 1;
	assert(drmOpen("psb", NULL) == -EACCES);
	fail_open = 0; fd = drmOpen("psb", NULL); other = drmOpen("pvrsrvkm", NULL);
	assert(fd > 0 && other > 0 && fd != other);
	assert(!drmDropMaster(fd) && drmDropMaster(-1) == -EBADF);
	assert(!drmCommandNone(fd, 0x15) && drmCommandNone(fd, 0x12) == -EOPNOTSUPP);
	assert(drmCommandWrite(-1, 0x12, &wire, 28) == -EBADF);
	assert(drmCommandWrite(fd, 0x12, &wire, 24) == -EOPNOTSUPP && !ioctls);
	assert(drmCommandWrite(fd, 0x13, &wire, 28) == -EOPNOTSUPP && !ioctls);
	assert(!drmCommandWrite(fd, 0x12, &wire, 28) && ioctls == 1);
	fail_ioctl = 1;
	assert(drmCommandWrite(fd, 0x12, &wire, 28) == -EOPNOTSUPP && ioctls == 2);
	fail_ioctl = 0; before = ioctls;
	wire.bridge_id = SGX545_COMMAND(99);
	assert(drmCommandWrite(fd, 0x12, &wire, 28) == -EINVAL && ioctls == before);
	wire.bridge_id = SGX545_COMMAND(0) ^ (1U << 16);
	assert(drmCommandWrite(fd, 0x12, &wire, 28) == -EINVAL && ioctls == before);
	wire.bridge_id = SGX545_COMMAND(68); wire.in_size = 8; wire.param_in = (unsigned)(unsigned long)input;
	input[1] = 1;
	assert(drmCommandWrite(fd, 0x12, &wire, 28) == -EOPNOTSUPP && ioctls == before);
	input[1] = 0;
	assert(!drmCommandWrite(fd, 0x12, &wire, 28) && ioctls == before + 1);
	wire.bridge_id = SGX545_COMMAND(6); wire.in_size = 24;
	input[2] = 0x11; input[3] = 0x400b; input[4] = 8192; input[5] = 4096;
	assert(!drmCommandWrite(fd, 0x12, &wire, 28) && ioctls == before + 2);
	wire.bridge_id = SGX545_COMMAND(12); wire.in_size = 8;
	input[0] = 0xfefefefe; input[1] = SGX545_CONNECT_PDUMP_PERSIST;
	assert(!drmCommandWrite(fd, 0x12, &wire, 28) && ioctls == before + 3);
	assert(input[0] == 0xfefefefe && input[1] == SGX545_CONNECT_PDUMP_PERSIST); /* logged, never rewritten */
	wire.bridge_id = SGX545_COMMAND(1); wire.in_size = 12;
	input[1] = 0; input[2] = SGX545_DEVICE_TYPE_UNKNOWN;
	assert(!drmCommandWrite(fd, 0x12, &wire, 28) && ioctls == before + 4);
	assert(input[0] == 0xfefefefe && input[1] == 0 && input[2] == SGX545_DEVICE_TYPE_UNKNOWN);
	wire.bridge_id = SGX545_COMMAND(27); wire.in_size = 8;
	wire.out_size = 16; wire.param_out = (unsigned)(unsigned long)(input + 800);
	memset(input + 800, 0, 16);
	assert(!drmCommandWrite(fd, 0x12, &wire, 28) && ioctls == before + 5);
	wire.in_size = 4097;
	assert(drmCommandWrite(fd, 0x12, &wire, 28) == -EINVAL);
	wire.in_size = 8; wire.param_in = 0xfffffffc;
	assert(drmCommandWrite(fd, 0x12, &wire, 28) == -EINVAL);
	requests = 4096; before = ioctls;
	assert(drmCommandWrite(fd, 0x12, &wire, 28) == -E2BIG && ioctls == before);
	requests = 0;
	input[0] = 0;
	assert(!drmCommandWrite(fd, 0x16, input, 4) && !input[0]);
	input[0] = 1;
	assert(drmCommandWrite(fd, 0x16, input, 4) == -EOPNOTSUPP);
	assert(drmClose(-1) == -EBADF);
	assert(!drmClose(fd));
	fail_close = 1;
	assert(drmClose(other) == -EINTR && !descriptor_count);
	fail_close = 0;
	handoff_tests(input);
	for (i=0;i<7;i++) {
		int result;
		fd=drmOpen("psb",NULL); runtime_live=1; sequence_count=0;
		memset(owner_calls,0,sizeof(owner_calls));
		owner_fail_op=i>=1 && i<=3 ? (int)i-1 : -1;
		owner_malformed_op=i>=4 ? (int)i-4 : -1;
		result=boot_owner_cycle(fd,10,200,20,11,11);
		assert(result==(i==0 ? 0 : i<=3 ? EIO : EPROTO));
		assert(owner_calls[0]==1 && owner_calls[2]==(i!=1));
		if(i==3) {
			struct sgx545_boot_owner cleanup={.services=10,.stage=200,.device=20,.operation=SGX545_OWNER_RELEASE};
			assert(owner_held); owner_fail_op=-1;
			assert(!transport_ioctl(fd,SGX545_IOCTL_BOOT_OWNER,&cleanup));
		}
		assert(!owner_held); runtime_live=0; assert(!drmClose(fd));
	}
	owner_fail_op=owner_malformed_op=-1;
	assert(drmCommandWrite(fd, 0x12, &wire, 28) == -EBADF);
	for (i = 0; i < 32; i++) assert(drmOpen("psb", NULL) > 0);
	before = opens;
	assert(drmOpen("psb", NULL) == -EMFILE && opens == before);
	while (descriptor_count) assert(!drmClose(descriptors[0]));
	assert(!munmap(input, 4096));
	puts("SGX initializer transport tests passed: validation/stage/query/abort, failure cleanup, private descriptors and hardware/finalization blocks");
	return 0;
}
