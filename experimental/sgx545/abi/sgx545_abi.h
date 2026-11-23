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

/* Intel 1.7 i386 ioctl and payload layouts. All members are 32-bit words,
 * including the two halves of CONNECT's 64-bit handle (aligned to 4 on i386).
 */
#define SGX545_IOCTL_SERVICES       0xc01c6440U
#define SGX545_IOCTL_SERVICES_ALIAS 0xc01c6452U
#define SGX545_COMMAND_BASE         0xc01c6700U
#define SGX545_COMMAND(number)      (SGX545_COMMAND_BASE | (number))
#define SGX545_CMD_ENUMERATE        0U
#define SGX545_CMD_ACQUIRE          1U
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
#define SGX545_MAX_HEAPS            32U
#define SGX545_UNDEFINED_HEAP       0xffffffffU
#define SGX545_BUFFER_WC            0x00004000U
#define SGX545_BUFFER_MULTI         0x00020000U
#define SGX545_BUFFER_MAX_BYTES     0x00100000U
/* Project diagnostic ioctl, separate from the Intel services command space. */
#define SGX545_IOCTL_BUFFER_CHECK   0xc0104701U
struct sgx545_buffer_check { __u32 services, handle, expected, fill; };

struct sgx545_connect_out { __u32 error, services_lo, services_hi; };
struct sgx545_device_id { __u32 type, device_class, index; };
struct sgx545_enumerate_out {
	__u32 error, count;
	struct sgx545_device_id devices[SGX545_MAX_DEVICES];
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
struct sgx545_map_info_out { __u32 error, offset, byte_offset, real_size, user_address; };

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
