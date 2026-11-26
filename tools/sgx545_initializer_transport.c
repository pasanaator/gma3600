// SPDX-License-Identifier: GPL-2.0-only
/* Private libdrm substitute for the unmodified Intel initializer. This is not
 * a DRM driver or a general libdrm implementation. It opens only our existing
 * prepare-only diagnostic bridge and never forwards a display/submit ioctl.
 */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "sgx545_abi.h"

#define EXPORT __attribute__((visibility("default")))
#ifndef SGX545_TRANSPORT_TEST
_Static_assert(sizeof(void *) == 4, "Intel initializer transport must be i386");
#define transport_open open
#define transport_close close
#define transport_ioctl ioctl
#endif

/* Each drmOpen gets its own bridge file: do not merge legacy connections. */
static int descriptors[32];
static unsigned aborted_services[32];
static unsigned descriptor_count;
static unsigned requests;

static int descriptor_slot(int fd)
{
	unsigned i;
	for (i = 0; i < descriptor_count; i++)
		if (descriptors[i] == fd) return (int)i;
	return -1;
}
static int owned(int fd) { return descriptor_slot(fd) >= 0; }

EXPORT const char *gma3600_sgx_transport_version(void) { return "0.9.0"; }

EXPORT int gma3600_sgx_transport_accepts_device(const char *name, const char *busid)
{
	/* Intel selects the integrated GPU by PCI address, with a NULL name.
	 * Named opens are also supported, but neither selector may identify a
	 * different driver/device. Never turn this into general DRM discovery. */
	return (name || busid) &&
		(!name || !strcmp(name, "psb") || !strcmp(name, "pvrsrvkm")) &&
		(!busid || !strcmp(busid, "pci:0000:00:02.0"));
}

EXPORT int drmOpen(const char *name, const char *busid)
{
	int fd;
	fprintf(stderr, "TRANSPORT open name=%s busid=%s\n", name ? name : "(null)", busid ? busid : "(null)");
	if (!gma3600_sgx_transport_accepts_device(name, busid)) {
		fprintf(stderr, "BLOCK open selector\n");
		return -EINVAL;
	}
	if (descriptor_count == 32) return -EMFILE;
	fd = transport_open("/dev/gma3600-sgx-bridge", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		int error = errno;
		fprintf(stderr, "TRANSPORT open_failed errno=%d\n", error);
		return -error;
	}
	aborted_services[descriptor_count] = 0;
	descriptors[descriptor_count++] = fd;
	fprintf(stderr, "TRANSPORT opened fd=%d\n", fd);
	return fd;
}

EXPORT int drmClose(int fd)
{
	unsigned i;
	int result;
	for (i = 0; i < descriptor_count; i++) {
		if (descriptors[i] != fd) continue;
		/* Linux releases the fd even when close reports EINTR. */
		descriptors[i] = descriptors[--descriptor_count];
		aborted_services[i] = aborted_services[descriptor_count];
		result = transport_close(fd);
		if (result < 0) return -errno;
		return 0;
	}
	return -EBADF;
}

EXPORT int drmDropMaster(int fd)
{
	/* There is no DRM master on this misc-device transport. */
	return owned(fd) ? 0 : -EBADF;
}

EXPORT int drmCommandNone(int fd, unsigned long command)
{
	fprintf(stderr, "TRANSPORT none command=%lu fd=%d\n", command, fd);
	if (!owned(fd)) return -EBADF;
	/* Original PVR_DRM_IS_MASTER_CMD. The kernel still checks initializer
	 * capability/ownership; no KMS master is acquired or released here. */
	return command == 0x15 ? 0 : -EOPNOTSUPP;
}

static void script_metadata(const char *name, const struct sgx545_init_command32 *commands, unsigned count)
{
	unsigned i;
	for (i = 0; i < count; i++) {
		if (commands[i].op == 2) {
			fprintf(stderr, "SCRIPT name=%s writes=%u halt=1\n", name, i);
			return;
		}
		if (commands[i].op != 1 || commands[i].offset & 3 || commands[i].offset >= 0x4000) {
			fprintf(stderr, "SCRIPT name=%s invalid_index=%u op=%u offset=%08x\n",
				name, i, commands[i].op, commands[i].offset);
			return;
		}
		/* Values are initializer data, never executed by this transport. Keep
		 * the ordered sequence for a separate firmware register-policy audit. */
		fprintf(stderr, "SCRIPT_WRITE name=%s index=%u offset=%08x value=%08x\n",
			name, i, commands[i].offset, commands[i].value);
	}
	fprintf(stderr, "SCRIPT name=%s writes=%u halt=0\n", name, count);
}

static int content_readback(int fd, unsigned services, unsigned stage,
	const unsigned *tokens, unsigned count)
{
	unsigned i, checked = 0, bytes = 0, nonzero = 0, populated = 0, changed = 0, mapped = 0;
	int error = 0;

	for (i = 0; i < count; i++) {
		struct sgx545_init_content content = {.services = services, .stage = stage, .index = i};
		if (transport_ioctl(fd, SGX545_IOCTL_INIT_CONTENT, &content) < 0) { error = errno; break; }
		if (content.services != services || content.stage != stage || content.index != i || content.token != tokens[i] ||
		    !content.bytes || content.bytes > SGX545_BUFFER_MAX_BYTES || content.nonzero_bytes > content.bytes ||
		    content.padding_nonzero > ((content.bytes + 4095) & ~4095U) - content.bytes ||
		    !content.address || (content.address & 4095) || !sgx545_device_effective_flags_valid(content.memory_flags) ||
		    (content.flags & ~(SGX545_CONTENT_CHANGED | SGX545_CONTENT_MAPPED))) { error = EPROTO; break; }
		fprintf(stderr, "CONTENT index=%u handle=%08x bytes=%u address=%08x memory_flags=%08x baseline=%08x current=%08x nonzero=%u padding_nonzero=%u changed=%u mapped=%u\n",
			i, content.token, content.bytes, content.address, content.memory_flags, content.baseline_hash, content.current_hash,
			content.nonzero_bytes, content.padding_nonzero, !!(content.flags & SGX545_CONTENT_CHANGED), !!(content.flags & SGX545_CONTENT_MAPPED));
		checked++; bytes += content.bytes; nonzero += content.nonzero_bytes; populated += content.nonzero_bytes != 0;
		mapped += !!(content.flags & SGX545_CONTENT_MAPPED);
		if ((content.flags & SGX545_CONTENT_CHANGED) || content.baseline_hash != content.current_hash) { changed++; error = EUCLEAN; break; }
	}
	fprintf(stderr, "CONTENT_SUMMARY checked=%u buffers=%u bytes=%u nonzero_bytes=%u populated=%u changed=%u errno=%d mapped=%u\n",
		checked, count, bytes, nonzero, populated, changed, error, mapped);
	return error;
}

/* Exercise the CPU-only handoff on the real initializer's payload. The legacy
 * command itself is NEVER forwarded, and failure is returned even after this
 * diagnostic cycle passes. Native initialization and script execution stay
 * unreachable. The kernel owns all authorization, validation and lifetimes. */
static int ownership_readback(int fd, unsigned services, unsigned stage, const unsigned *roles, unsigned buffers, unsigned pages)
{
	unsigned i, checked = 0, total = 0;
	int error = 0;
	for (i = 0; i < buffers; i++) {
		struct sgx545_bm_info info = {.services = services, .stage = stage, .index = i};
		if (transport_ioctl(fd, SGX545_IOCTL_BM_INFO, &info) < 0) { error = errno; break; }
		if (info.services != services || info.stage != stage || info.index != i || info.token != roles[i] || !info.address ||
		    !info.bytes || info.bytes > SGX545_BUFFER_MAX_BYTES || info.pages != (info.bytes + 4095) / 4096 ||
		    info.alignment < 4096 || info.alignment > SGX545_BUFFER_MAX_BYTES || (info.alignment & (info.alignment - 1)) ||
		    (info.address & (info.alignment - 1)) || !sgx545_device_effective_flags_valid(info.memory_flags) ||
		    info.owned != 1 || info.registered != 1 || info.attached != 1) { error = EPROTO; break; }
		fprintf(stderr, "OWNED index=%u handle=%08x address=%08x bytes=%u pages=%u memory_flags=%08x alignment=%u owned=%u registered=%u attached=%u\n",
			info.index, info.token, info.address, info.bytes, info.pages, info.memory_flags, info.alignment,
			info.owned, info.registered, info.attached);
		checked++; total += info.pages;
	}
	if (!error && total != pages) error = EPROTO;
	fprintf(stderr, "OWNED_SUMMARY checked=%u buffers=%u pages=%u errno=%d allocator=native_bm\n", checked, buffers, total, error);
	return error;
}

static int bm_readback(int fd, unsigned services, unsigned stage, const unsigned *roles, unsigned buffers, unsigned pages)
{
	unsigned index, checked = 0, total_pages = 0;
	int error = 0;
	for (index = 0; index < buffers; index++) {
		struct sgx545_bm_check check = {.services = services, .stage = stage, .index = index};
		if (transport_ioctl(fd, SGX545_IOCTL_BM_CHECK, &check) < 0) { error = errno; break; }
		if (check.services != services || check.stage != stage || check.index != index || check.token != roles[index] ||
		    !check.original_address || !check.alias_address || check.original_address == check.alias_address ||
		    (check.original_address & 4095) || (check.alias_address & 4095) || !check.bytes || check.bytes > SGX545_BUFFER_MAX_BYTES ||
		    check.pages != (check.bytes + 4095) / 4096 || !sgx545_device_effective_flags_valid(check.memory_flags) ||
		    check.passes != 2 || check.pt_before != check.pt_after) { error = EPROTO; break; }
		fprintf(stderr, "BM index=%u handle=%08x original=%08x alias=%08x bytes=%u pages=%u memory_flags=%08x passes=%u pt_before=%u pt_after=%u\n",
			check.index, check.token, check.original_address, check.alias_address, check.bytes, check.pages,
			check.memory_flags, check.passes, check.pt_before, check.pt_after);
		checked++; total_pages += check.pages;
	}
	if (!error && total_pages != pages) error = EPROTO;
	fprintf(stderr, "BM_SUMMARY checked=%u buffers=%u pages=%u errno=%d gpu_execution=disabled\n", checked, buffers, total_pages, error);
	return error;
}

static int runtime_handoff(int fd, unsigned services, unsigned stage, unsigned device,
	const unsigned *roles, unsigned buffers, unsigned pages)
{
	struct sgx545_native_runtime request = {.state = {.services = services, .stage = stage, .device = device}};
	struct sgx545_native_runtime reply = request;
	int error = 0, bound = 0, queried = 0, unbound = 0;

	if (transport_ioctl(fd, SGX545_IOCTL_NATIVE_RUNTIME, &reply) < 0) error = errno;
	else {
		bound = 1;
		if (reply.state.services != services || reply.state.stage != stage || reply.state.device != device ||
		    reply.operation != SGX545_NATIVE_BIND || reply.reserved || reply.state.bound != 1 || reply.state.restored ||
		    reply.state.flags != SGX545_NATIVE_CCB_ROUNDTRIP || reply.state.buffers != buffers || reply.state.pages != pages ||
		    reply.state.ccb_bytes != 8192 || reply.state.slots != 256 || reply.state.read_offset >= 256 ||
		    reply.state.read_offset != reply.state.write_offset) error = EPROTO;
	}
	if (!error) {
		struct sgx545_native_cycle previous = reply.state;
		reply = request; reply.operation = SGX545_NATIVE_QUERY;
		if (transport_ioctl(fd, SGX545_IOCTL_NATIVE_RUNTIME, &reply) < 0) error = errno;
		else if (reply.operation != SGX545_NATIVE_QUERY || reply.reserved ||
			 memcmp(&reply.state, &previous, sizeof(previous))) error = EPROTO;
		else queried = 1;
		if (!error) error = ownership_readback(fd, services, stage, roles, buffers, pages);
		if (!error) error = bm_readback(fd, services, stage, roles, buffers, pages);
		if (!error) {
			queried = 0;
			reply = request; reply.operation = SGX545_NATIVE_QUERY;
			if (transport_ioctl(fd, SGX545_IOCTL_NATIVE_RUNTIME, &reply) < 0) error = errno;
			else if (reply.operation != SGX545_NATIVE_QUERY || reply.reserved ||
				 memcmp(&reply.state, &previous, sizeof(previous))) error = EPROTO;
			else queried = 1;
		}
		if (!error) error = content_readback(fd, services, stage, roles, buffers);
	}
	/* Always unbind a delivered BIND, even on a malformed reply, query error or
	 * content failure. The caller additionally aborts the entire stage. */
	if (bound) {
		reply = request; reply.operation = SGX545_NATIVE_UNBIND;
		if (transport_ioctl(fd, SGX545_IOCTL_NATIVE_RUNTIME, &reply) < 0) { if (!error) error = errno; }
		else if (reply.state.services != services || reply.state.stage != stage || reply.state.device != device ||
			 reply.operation != SGX545_NATIVE_UNBIND || reply.reserved || reply.state.flags || reply.state.bound ||
			 reply.state.restored != 1 || reply.state.buffers != buffers || reply.state.pages != pages ||
			 reply.state.ccb_bytes || reply.state.slots || reply.state.read_offset || reply.state.write_offset) {
			if (!error) error = EPROTO;
		} else unbound = 1;
	}
	fprintf(stderr, "RUNTIME bound=%d queried=%d unbound=%d buffers=%u pages=%u errno=%d gpu_execution=disabled\n",
		bound, queried, unbound, buffers, pages, error);
	return error;
}

static int initializer_handoff(int fd, const struct sgx545_bridge32 *wire)
{
	struct sgx545_init2_in32 request;
	struct sgx545_init_stage validation, stage;
	struct sgx545_init_requirements caps = {0};
	struct sgx545_init_control control = {0};
	struct sgx545_native_cycle native = {0};
	unsigned roles[SGX545_INIT_MAX_BUFFERS], token_count = SGX545_INIT_ROLES, i, buffers = 0, pages = 0;
	int slot = descriptor_slot(fd), error = 0, validated = 0, staged = 0, queried = 0, aborted = 0;

	if (wire->in_size != sizeof(request) || wire->out_size != sizeof(struct sgx545_init2_out32))
		return -EINVAL;
	if (aborted_services[slot]) return -EALREADY;
	memcpy(&request, (void *)(unsigned long)wire->param_in, sizeof(request));
	fprintf(stderr, "INIT2 reserved=%08x device=%08x build_options=%08x\n",
		request.flags, request.device, request.info.build_options);
	/* The original bridge's first word is reserved, not an input flag. Only
	 * this private copy is normalized for the stricter project-only ioctl. */
	request.flags = 0;
	roles[0] = request.info.ccb; roles[1] = request.info.ccb_control; roles[2] = request.info.event_kicker;
	roles[3] = request.info.host_control; roles[4] = request.info.ta3d_control; roles[5] = request.info.misc;
	roles[6] = request.info.hwperf; roles[7] = request.info.ta_signature; roles[8] = request.info.signature_3d;
	caps.services = wire->services; caps.device = request.device;
	if (transport_ioctl(fd, SGX545_IOCTL_INIT_REQUIREMENTS, &caps) < 0) { error = errno; goto done; }
	fprintf(stderr, "INIT2_EXPECT build_options=%08x mask=%08x\n", caps.build_options, caps.build_option_mask);
	for (i = 0; i < SGX545_INIT_ROLES; i++)
		fprintf(stderr, "ROLE index=%u handle=%08x minimum_bytes=%u\n", i, roles[i], caps.minimum_bytes[i]);
	for (i = 0; i < 13; i++)
		fprintf(stderr, "STRUCT index=%u supplied=%u required=%u\n", i, request.info.struct_sizes[i], caps.struct_sizes[i]);
	for (i = 0; i < 18; i++)
		if (request.info.mem_handles[i]) {
			fprintf(stderr, "INIT_MEMORY index=%u handle=%08x\n", i, request.info.mem_handles[i]);
			roles[token_count++] = request.info.mem_handles[i];
		}
	script_metadata("part1", request.info.scripts.part1, 64);
	script_metadata("part2", request.info.scripts.part2, 64);
	script_metadata("deinit", request.info.scripts.deinit, 16);
	fprintf(stderr, "BOOT_CONTROL task0=%08x task1=%08x clock0=%08x clock1=%08x status_reg=%08x status_mask=%08x cache=%08x\n",
		request.info.edm_task[0], request.info.edm_task[1], request.info.clock_control[0], request.info.clock_control[1],
		request.info.clock_status_reg, request.info.clock_status_mask, request.info.cache_control);
	for (i = 0; i < 11; i++)
		fprintf(stderr, "BOOT_KICK index=%u value=%08x\n", i, request.info.host_kick[i]);
	for (i = 0; i < 24; i++)
		fprintf(stderr, "BOOT_DATA index=%u value=%08x\n", i, request.info.dev_data[i]);
	validation = (struct sgx545_init_stage){.services = wire->services, .device = request.device,
		.request = (unsigned)(unsigned long)&request, .request_bytes = sizeof(request)};
	stage = validation;
	if (transport_ioctl(fd, SGX545_IOCTL_INIT_VALIDATE, &validation) < 0) { error = errno; goto done; }
	validated = 1; buffers = validation.buffers; pages = validation.pages;
	if (validation.stage || validation.flags || buffers != token_count || buffers < SGX545_INIT_ROLES ||
	    buffers > SGX545_INIT_MAX_BUFFERS || pages < buffers) { error = EPROTO; goto done; }
	if (transport_ioctl(fd, SGX545_IOCTL_INIT_STAGE, &stage) < 0) { error = errno; goto done; }
	staged = 1;
	/* STAGE has already ended this initializer attempt in the kernel. Keep
	 * that fact even if QUERY or ABORT fails; never issue a second native abort. */
	aborted_services[slot] = wire->services;
	control.services = wire->services; control.stage = stage.stage;
	if (!stage.stage || stage.flags || stage.buffers != buffers || stage.pages != pages) error = EPROTO;
	else if (transport_ioctl(fd, SGX545_IOCTL_INIT_CONTROL, &control) < 0) error = errno;
	else if (control.services != wire->services || control.stage != stage.stage || control.operation != SGX545_STAGE_QUERY ||
		 control.buffers != buffers || control.pages != pages || control.flags) error = EPROTO;
	else queried = 1;
	if (queried) {
		native.services = wire->services; native.stage = stage.stage; native.device = request.device;
		if (transport_ioctl(fd, SGX545_IOCTL_NATIVE_CYCLE, &native) < 0) error = errno;
		else if (native.services != wire->services || native.stage != stage.stage || native.device != request.device ||
			 native.flags != SGX545_NATIVE_CCB_ROUNDTRIP || native.bound != 1 || native.restored != 1 ||
			 native.buffers != buffers || native.pages != pages || native.ccb_bytes != 8192 || native.slots != 256 ||
			 native.read_offset >= native.slots || native.write_offset != native.read_offset) error = EPROTO;
		if (error) fprintf(stderr, "NATIVE errno=%d\n", error);
		else {
			fprintf(stderr, "NATIVE bound=%u restored=%u ccb_bytes=%u slots=%u read=%u write=%u roundtrip=1 buffers=%u pages=%u gpu_execution=disabled\n",
				native.bound, native.restored, native.ccb_bytes, native.slots, native.read_offset, native.write_offset, native.buffers, native.pages);
			error = runtime_handoff(fd, wire->services, stage.stage, request.device, roles, buffers, pages);
		}
	}
	/* Always attempt cleanup after a delivered STAGE, including query failure. */
	control = (struct sgx545_init_control){.services = wire->services, .stage = stage.stage, .operation = SGX545_STAGE_ABORT};
	if (transport_ioctl(fd, SGX545_IOCTL_INIT_CONTROL, &control) < 0) error = errno;
	else if (control.services != wire->services || control.stage != stage.stage || control.operation != SGX545_STAGE_ABORT ||
		 control.buffers || control.pages || control.flags) error = EPROTO;
	else aborted = 1;
done:
	fprintf(stderr, "HANDOFF validated=%d staged=%d queried=%d aborted=%d buffers=%u pages=%u errno=%d gpu_execution=disabled\n",
		validated, staged, queried, aborted, buffers, pages, error);
	return error ? -error : -EOPNOTSUPP;
}

EXPORT int drmCommandWrite(int fd, unsigned long command, void *data, unsigned long size)
{
	struct sgx545_bridge32 *wire = data;
	unsigned number;
	int result;

	if (!owned(fd)) return -EBADF;
	if (++requests > 4096) return -E2BIG;
	/* PVR_DRM_UNPRIV_INIT_SUCCESFUL: prepare-only never reports success. */
	if (command == 0x16 && size == 4 && data && !*(unsigned *)data) {
		*(unsigned *)data = 0;
		fprintf(stderr, "TRANSPORT init_successful=0\n");
		return 0;
	}
	if (command != 0x12 || size != sizeof(*wire) || !data) {
		fprintf(stderr, "BLOCK transport command=%lu bytes=%lu\n", command, size);
		return -EOPNOTSUPP;
	}
	if (wire->size != sizeof(*wire) || (wire->bridge_id & ~255U) != SGX545_COMMAND_BASE ||
	    wire->in_size > SGX545_BRIDGE_BUFFER_MAX || wire->out_size > SGX545_BRIDGE_BUFFER_MAX ||
	    (wire->in_size && (!wire->param_in || wire->in_size - 1 > ~wire->param_in)) ||
	    (wire->out_size && (!wire->param_out || wire->out_size - 1 > ~wire->param_out)))
		return -EINVAL;
	number = wire->bridge_id & 255;
	fprintf(stderr, "BRIDGE command=%u in=%u out=%u\n", number, wire->in_size, wire->out_size);
	if (number == SGX545_CMD_CONNECT && wire->in_size == 8) {
		const unsigned *in = (void *)(unsigned long)wire->param_in;
		fprintf(stderr, "CONNECT reserved=%08x service_flags=%08x\n", in[0], in[1]);
	} else if (number == SGX545_CMD_ACQUIRE && wire->in_size == 12) {
		const unsigned *in = (void *)(unsigned long)wire->param_in;
		fprintf(stderr, "ACQUIRE reserved=%08x index=%u type=%u\n", in[0], in[1], in[2]);
	} else if (wire->in_size >= 4) {
		fprintf(stderr, "INPUT command=%u reserved=%08x\n", number,
			*(unsigned *)(unsigned long)wire->param_in);
	}
	/* Record allocation metadata, never firmware or buffer contents. */
	if (number == SGX545_CMD_ALLOC_DEVICE && wire->in_size == 24) {
		unsigned *in = (void *)(unsigned long)wire->param_in;
		fprintf(stderr, "ALLOC heap=%08x flags=%08x bytes=%u alignment=%u\n", in[2], in[3], in[4], in[5]);
	}
	switch (number) {
	case SGX545_CMD_ENUMERATE: case SGX545_CMD_ACQUIRE:
	case SGX545_CMD_CONNECT: case SGX545_CMD_DISCONNECT:
	case SGX545_CMD_INIT_CONNECT: case SGX545_CMD_INIT_INFO:
	case SGX545_CMD_ALLOC_SHARED: case SGX545_CMD_FREE_SHARED:
	case SGX545_CMD_MAP_INFO: case SGX545_CMD_RELEASE_MAP: case SGX545_CMD_ALLOC_DEVICE: case SGX545_CMD_FREE_DEVICE:
		break;
	case SGX545_CMD_INIT_DISCONNECT:
		if (wire->in_size != 8 || !wire->param_in) return -EINVAL;
		if (((unsigned *)(unsigned long)wire->param_in)[1]) {
			fprintf(stderr, "BLOCK successful initializer finalization\n");
			return -EOPNOTSUPP;
		}
		if (aborted_services[descriptor_slot(fd)] == wire->services && wire->services) {
			if (wire->out_size != 8) return -EINVAL;
			memset((void *)(unsigned long)wire->param_out, 0, 8);
			fprintf(stderr, "TRANSPORT init_abort=already_performed_by_stage\n");
			return 0;
		}
		break;
	case 99:
		return initializer_handoff(fd, wire);
	default:
		fprintf(stderr, "BLOCK bridge command=%u\n", number);
		return -EOPNOTSUPP;
	}
	result = transport_ioctl(fd, SGX545_IOCTL_SERVICES, wire);
	if (result < 0) {
		result = -errno;
		fprintf(stderr, "RESULT command=%u errno=%d\n", number, -result);
	} else if (wire->out_size >= 4) {
		fprintf(stderr, "RESULT command=%u payload_error=%u\n", number,
			*(unsigned *)(unsigned long)wire->param_out);
		if (number == SGX545_CMD_DISCONNECT && !*(unsigned *)(unsigned long)wire->param_out)
			aborted_services[descriptor_slot(fd)] = 0;
		if (number == SGX545_CMD_RELEASE_MAP && wire->in_size == 8 && wire->out_size == sizeof(struct sgx545_release_map_out)) {
			const struct sgx545_release_map_out *out = (void *)(unsigned long)wire->param_out;
			if (!out->error)
				fprintf(stderr, "MAP_RELEASE handle=%08x unmap=%u address=%08x bytes=%u\n",
					((unsigned *)(unsigned long)wire->param_in)[1], out->unmap, out->user_address, out->real_size);
		}
		if (number == SGX545_CMD_ALLOC_DEVICE && wire->out_size == sizeof(struct sgx545_alloc_device_out)) {
			const struct sgx545_alloc_device_out *out = (void *)(unsigned long)wire->param_out;
			if (!out->error)
				fprintf(stderr, "ALLOCATED handle=%08x address=%08x bytes=%u flags=%08x\n",
					out->kernel_mem, out->mem.device_address, out->mem.alloc_size, out->mem.flags);
		}
	}
	return result;
}
