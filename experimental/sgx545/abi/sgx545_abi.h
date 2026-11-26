/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA_SGX545_ABI_H
#define GMA_SGX545_ABI_H

#ifdef SGX545_FREESTANDING
typedef __UINT32_TYPE__ __u32;
typedef __UINT64_TYPE__ __u64;
#else
#include <linux/types.h>
#endif

/* Intel DDK 1.7.862890 i386 PVRSRV_BRIDGE_PACKAGE. Never cast this to the
 * native DDK structure: sizeof(native) is 48 on amd64, versus 28 here.
 * These fields are little endian, as on the supported x86 platforms.
 */
struct sgx545_bridge32 {
	__u32 bridge_id;
	__u32 size;
	__u32 param_in;
	__u32 in_size;
	__u32 param_out;
	__u32 out_size;
	__u32 services;
};

/* Internal normalized envelope. Handles remain IDs, never kernel pointers.
 * Nested payloads still require command-specific translation and validation.
 */
struct sgx545_bridge_request {
	__u64 param_in;
	__u64 param_out;
	__u32 bridge_id;
	__u32 in_size;
	__u32 out_size;
	__u32 services;
};

#define SGX545_BRIDGE32_SIZE 28
#define SGX545_BRIDGE_BUFFER_MAX 0x1000

/* Intel i386 ioctl and payload layouts. All members are 32-bit words.
 * The original Intel CONNECT reply is 8 bytes; the community compatibility
 * reply below adds a high handle word. Both forms are supported.
 */
#define SGX545_IOCTL_SERVICES       0xc01c6440U
#define SGX545_IOCTL_SERVICES_ALIAS 0xc01c6452U
#define SGX545_COMMAND_BASE         0xc01c6700U
#define SGX545_COMMAND(number)      (SGX545_COMMAND_BASE | (number))
#define SGX545_CMD_ENUMERATE        0U
#define SGX545_CMD_ACQUIRE          1U
#define SGX545_CMD_ALLOC_DEVICE     6U
#define SGX545_CMD_FREE_DEVICE      7U
#define SGX545_CMD_MAP_INFO         11U
#define SGX545_CMD_RELEASE_MAP      27U
#define SGX545_CMD_CONNECT          12U
#define SGX545_CMD_DISCONNECT       13U
#define SGX545_CMD_INIT_CONNECT     67U
#define SGX545_CMD_ALLOC_SHARED     63U
#define SGX545_CMD_FREE_SHARED      64U
#define SGX545_CMD_INIT_DISCONNECT  68U
#define SGX545_CMD_INIT_INFO        98U
#define SGX545_MAX_DEVICES          16U
#define SGX545_DEVICE_TYPE_UNKNOWN  0U
#define SGX545_DEVICE_TYPE          7U
#define SGX545_CONNECT_PDUMP_PERSIST 1U
#define SGX545_CONNECT_PDUMP_ACTIVE  2U
#define SGX545_CONNECT_PDUMP_FLAGS   3U
#define SGX545_MAX_HEAPS            32U
#define SGX545_UNDEFINED_HEAP       0xffffffffU
#define SGX545_BUFFER_WC            0x00004000U
#define SGX545_BUFFER_CACHE_MASK    0x00007000U
#define SGX545_BUFFER_MULTI         0x00020000U
#define SGX545_BUFFER_MAX_BYTES     0x00100000U
#define SGX545_DEVICE_FLAGS        (SGX545_BUFFER_WC | 0x0000000bU) /* RW, no sync */
#define SGX545_RAM_BACKED           0x00000200U
#define SGX545_DEVICE_COHERENT      0x00000004U
#define SGX545_DEVICE_EDM_PROTECT   0x00000040U
#define SGX545_DEVICE_ZERO          0x00000080U
#define SGX545_DEVICE_READ          0x00000001U
#define SGX545_DEVICE_WRITE         0x00000002U
#define SGX545_DEVICE_ACCESS        (SGX545_DEVICE_READ | SGX545_DEVICE_WRITE)
#define SGX545_DEVICE_NO_SYNC       0x00000008U
#define SGX545_DEVICE_MMU_FLAGS     (SGX545_DEVICE_ACCESS | SGX545_DEVICE_COHERENT | SGX545_DEVICE_EDM_PROTECT)
#define SGX545_SYSMEM_NONCONTIG     0x02000000U
#define SGX545_HEAP_FLAGS           (SGX545_BUFFER_WC | SGX545_RAM_BACKED | SGX545_BUFFER_MULTI | SGX545_SYSMEM_NONCONTIG)
#define SGX545_INTEL_INIT_FLAGS     (SGX545_DEVICE_ACCESS | SGX545_DEVICE_NO_SYNC | SGX545_DEVICE_COHERENT | SGX545_DEVICE_EDM_PROTECT)
/* CPU cache mode may be omitted and inherited from a validated WC heap.
 * MULTI is already an attribute of these heaps; it does not publish a handle
 * or bypass this diagnostic bridge's per-file/process ownership checks. */
static inline int sgx545_device_flags_valid(__u32 flags)
{
	/* Access bits describe GPU permissions. CPU mappings stay writable so
	 * the initializer can populate GPU-read-only code and data buffers. */
	return (flags & SGX545_DEVICE_NO_SYNC) && (flags & SGX545_DEVICE_ACCESS) &&
		!(flags & ~(SGX545_DEVICE_FLAGS | SGX545_DEVICE_COHERENT |
			SGX545_DEVICE_EDM_PROTECT | SGX545_DEVICE_ZERO | SGX545_RAM_BACKED | SGX545_BUFFER_MULTI));
}
static inline int sgx545_device_effective_flags_valid(__u32 flags)
{
	__u32 required = SGX545_BUFFER_WC | SGX545_RAM_BACKED | SGX545_BUFFER_MULTI;
	return (flags & required) == required &&
		sgx545_device_flags_valid(flags & ~(SGX545_BUFFER_MULTI | SGX545_SYSMEM_NONCONTIG));
}
/* Project diagnostic ioctl, separate from the Intel services command space. */
#define SGX545_IOCTL_BUFFER_CHECK   0xc0104701U
struct sgx545_buffer_check { __u32 services, handle, expected, fill; };
#define SGX545_IOCTL_MMU_CHECK      0xc0284702U
#define SGX545_MMU_CROSS_BOUNDARY   1U
struct sgx545_mmu_check {
	__u32 services, device, heap, buffer, flags;
	/* The reported device address has already been unmapped on return. */
	__u32 device_address, checked_pages, pt_peak, pt_after, scatter_breaks;
};
#define SGX545_IOCTL_DEVICE_CHECK   0xc0184703U
struct sgx545_device_check { __u32 services, device, buffer, address, pages, page_tables; };

struct sgx545_connect_out { __u32 error, services_lo, services_hi; };
struct sgx545_device_id { __u32 type, device_class, index; };
struct sgx545_enumerate_out {
	__u32 error, count;
	struct sgx545_device_id devices[SGX545_MAX_DEVICES];
};
/* Original Intel identifiers include two PDump name pointers. Never expose
 * native addresses: these unused i386 pointer slots are always zero. */
struct sgx545_intel_device_id32 {
	__u32 type, device_class, index, pdump_device_name, pdump_register_name;
};
struct sgx545_intel_enumerate_out32 {
	__u32 error, count;
	struct sgx545_intel_device_id32 devices[SGX545_MAX_DEVICES];
};
struct sgx545_handle_out { __u32 error, handle; };
struct sgx545_heap32 { __u32 id, handle, base, size, attributes, tile_stride; };
struct sgx545_init_info_out {
	__u32 error, page_directory;
	struct sgx545_heap32 heaps[SGX545_MAX_HEAPS];
};
struct sgx545_client_mem32 {
	__u32 linear, kernel_linear, device_address, physical_address;
	__u32 flags, client_flags, alloc_size, sync_info;
	__u32 mapping, kernel_mem, resource, next;
};
struct sgx545_alloc_shared_out {
	__u32 error, kernel_mem;
	struct sgx545_client_mem32 mem;
};
struct sgx545_alloc_device_out {
	__u32 error, kernel_mem;
	struct sgx545_client_mem32 mem;
	__u32 sync[5]; /* NO_SYNCOBJ: all fields remain zero. */
};
struct sgx545_map_info_out { __u32 error, offset, byte_offset, real_size, user_address; };
struct sgx545_release_map_out { __u32 error, unmap, user_address, real_size; };

/* Audited Intel 20120717 release initializer layout: i386, SGX545, HWPERF,
 * recovery, no PDUMP/debug. This is a wire model only; command 99 is disabled.
 * Never cast it to SGX_BRIDGE_INIT_INFO, whose handles are native pointers.
 */
struct sgx545_init_command32 { __u32 op, offset, value; };
struct sgx545_init_scripts32 {
	struct sgx545_init_command32 part1[64], part2[64], deinit[16];
};
struct sgx545_init2_info32 {
	__u32 ccb, ccb_control, event_kicker, host_control, ta3d_control, misc;
	__u32 host_kick[11];
	struct sgx545_init_scripts32 scripts;
	__u32 build_options, struct_sizes[13];
	__u32 hwperf, ta_signature, signature_3d;
	__u32 edm_task[2], clock_control[2], clock_status_reg, clock_status_mask, cache_control;
	__u32 dev_data[24], mem_handles[18];
};
struct sgx545_init2_in32 { __u32 flags, device; struct sgx545_init2_info32 info; };
struct sgx545_init2_out32 { __u32 error, kernel_build_options; };

/* Project-only initializer staging: no legacy finalization or GPU execution. */
#define SGX545_INIT_ROLES 9U
#define SGX545_INIT_MAX_BUFFERS 27U
#define SGX545_IOCTL_INIT_REQUIREMENTS 0xc0684704U
struct sgx545_init_requirements {
	__u32 services, device, build_options, build_option_mask;
	__u32 minimum_bytes[SGX545_INIT_ROLES], struct_sizes[13];
};
#define SGX545_IOCTL_INIT_STAGE 0xc0204705U
/* Same envelope as STAGE, but no token, ownership or initializer state changes. */
#define SGX545_IOCTL_INIT_VALIDATE 0xc0204707U
struct sgx545_init_stage {
	__u32 services, device, request, request_bytes;
	__u32 stage, buffers, pages, flags;
};
#define SGX545_IOCTL_INIT_CONTROL 0xc0184706U
#define SGX545_STAGE_QUERY 0U
#define SGX545_STAGE_ABORT 1U
struct sgx545_init_control { __u32 services, stage, operation, buffers, pages, flags; };

/* Read-only CPU content observation. FNV-1a is a change detector, not firmware
 * authentication. Existing user mappings may still modify staged RAM. */
#define SGX545_IOCTL_INIT_CONTENT 0xc0304708U
#define SGX545_CONTENT_CHANGED 1U
#define SGX545_CONTENT_MAPPED 2U
struct sgx545_init_content {
	__u32 services, stage, index, token, bytes, address, memory_flags;
	__u32 baseline_hash, current_hash, nonzero_bytes, padding_nonzero, flags;
};
/* Audited CPU prefix of native initialization, immediately unbound/restored.
 * No power, registration, script, command publication or hardware kick. */
#define SGX545_IOCTL_NATIVE_CYCLE 0xc0304709U
#define SGX545_NATIVE_CCB_ROUNDTRIP 1U
struct sgx545_native_cycle {
	__u32 services, stage, device, flags, buffers, pages;
	__u32 ccb_bytes, slots, read_offset, write_offset, bound, restored;
};
/* CPU binding lease. All output fields and reserved must be zero on input.
 * This does not register BM objects or enable hardware execution. */
#define SGX545_IOCTL_NATIVE_RUNTIME 0xc038470aU
#define SGX545_NATIVE_BIND 0U
#define SGX545_NATIVE_QUERY 1U
#define SGX545_NATIVE_UNBIND 2U
struct sgx545_native_runtime {
	struct sgx545_native_cycle state;
	__u32 operation, reserved;
};
/* Temporary kernel-only native BM aliases; no device addresses are replaced. */
#define SGX545_IOCTL_BM_CHECK 0xc030470bU
struct sgx545_bm_check {
	__u32 services, stage, index, token, original_address, alias_address;
	__u32 bytes, pages, memory_flags, passes, pt_before, pt_after;
};
/* Verify retained native BM/OS ownership of the original allocation. */
#define SGX545_IOCTL_BM_INFO 0xc030470cU
struct sgx545_bm_info {
	__u32 services, stage, index, token, address, bytes, pages, memory_flags;
	__u32 alignment, owned, registered, attached;
};
struct sgx545_content_measure { __u32 hash, nonzero, padding_nonzero, mapped_vmas; };
static inline struct sgx545_content_measure sgx545_measure_content(const void *linear,
	__u32 bytes, __u32 mapped_bytes)
{
	const volatile unsigned char *data = linear;
	struct sgx545_content_measure result = {2166136261U, 0, 0, 0};
	__u32 i;
	for (i = 0; i < mapped_bytes; i++) {
		unsigned char value = data[i];
		if (i < bytes) {
			result.hash = (result.hash ^ value) * 16777619U;
			result.nonzero += value != 0;
		} else result.padding_nonzero += value != 0;
	}
	return result;
}

/* Diagnostic subset, scoped to an open file. Backend objects never cross ABI. */
struct sgx545_session {
	__u32 services, device, device_index, device_type;
	__u32 initializer, init_info;
};
struct sgx545_bridge_ops {
	int (*connect)(void *ctx, __u32 *token);
	int (*enumerate)(void *ctx, struct sgx545_enumerate_out *out);
	int (*acquire)(void *ctx, __u32 index, __u32 type, __u32 *token);
	int (*release_device)(void *ctx);
	int (*disconnect)(void *ctx);
	int (*copy_in)(void *ctx, void *dst, __u64 src, __u32 size);
	int (*copy_out)(void *ctx, __u64 dst, const void *src, __u32 size);
	int (*init_connect)(void *ctx);
	int (*init_abort)(void *ctx, int rollback);
	int (*init_info)(void *ctx, struct sgx545_init_info_out *out);
	int (*release_init_info)(void *ctx);
	int (*alloc_shared)(void *ctx, __u32 flags, __u32 bytes,
		struct sgx545_alloc_shared_out *out);
	int (*free_shared)(void *ctx, __u32 token);
	int (*map_info)(void *ctx, __u32 token, struct sgx545_map_info_out *out);
	int (*release_map)(void *ctx, __u32 token, struct sgx545_release_map_out *out);
	/* Undo only a successful MAP_INFO/RELEASE_MAP whose reply was not delivered. */
	int (*undo_map)(void *ctx, __u32 token, int release);
	int (*alloc_device)(void *ctx, __u32 heap, __u32 flags, __u32 bytes,
		__u32 alignment, struct sgx545_alloc_device_out *out);
	int (*free_device)(void *ctx, __u32 token);
};

/* Backend: zero=success, positive=DDK error, negative=Linux errno.
 * Caller serializes the session/backend. Newly created resources are rolled
 * back on a failed output copy. DISCONNECT commits before its output copy.
 * init_abort(rollback=1) undoes an undelivered claim without ending an attempt;
 * rollback=0 ends it. Initializer INFO is cached; release_init_info rolls back
 * only a newly created batch. The backend owns partial failures until close.
 */
int sgx545_bridge_execute(struct sgx545_session *session,
	const struct sgx545_bridge_ops *ops, void *ctx,
	const struct sgx545_bridge_request *request);
int sgx545_session_close(struct sgx545_session *session,
	const struct sgx545_bridge_ops *ops, void *ctx);

/* Accepts a copied, naturally aligned envelope, never a userspace pointer.
 * On failure the output is zeroed. Does not dereference param_in/param_out;
 * future callers must still use access_ok/copy_from_user/copy_to_user.
 */
int sgx545_bridge32_decode(const struct sgx545_bridge32 *wire,
			 unsigned int copied_size,
			 struct sgx545_bridge_request *request);

#endif
