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
#define SGX545_CMD_CONNECT          12U
#define SGX545_CMD_DISCONNECT       13U
#define SGX545_CMD_INIT_CONNECT     67U
#define SGX545_CMD_ALLOC_SHARED     63U
#define SGX545_CMD_FREE_SHARED      64U
#define SGX545_CMD_INIT_DISCONNECT  68U
#define SGX545_CMD_INIT_INFO        98U
#define SGX545_MAX_DEVICES          16U
#define SGX545_DEVICE_TYPE          7U
#define SGX545_CONNECT_PDUMP_PERSIST 1U
#define SGX545_CONNECT_PDUMP_ACTIVE  2U
#define SGX545_CONNECT_PDUMP_FLAGS   3U
#define SGX545_MAX_HEAPS            32U
#define SGX545_UNDEFINED_HEAP       0xffffffffU
#define SGX545_BUFFER_WC            0x00004000U
#define SGX545_BUFFER_MULTI         0x00020000U
#define SGX545_BUFFER_MAX_BYTES     0x00100000U
#define SGX545_DEVICE_FLAGS        (SGX545_BUFFER_WC | 0x0000000bU) /* RW, no sync */
#define SGX545_RAM_BACKED           0x00000200U
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
struct sgx545_init_stage {
	__u32 services, device, request, request_bytes;
	__u32 stage, buffers, pages, flags;
};
#define SGX545_IOCTL_INIT_CONTROL 0xc0184706U
#define SGX545_STAGE_QUERY 0U
#define SGX545_STAGE_ABORT 1U
struct sgx545_init_control { __u32 services, stage, operation, buffers, pages, flags; };

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
