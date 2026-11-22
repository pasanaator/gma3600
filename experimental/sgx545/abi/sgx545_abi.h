/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA_SGX545_ABI_H
#define GMA_SGX545_ABI_H

#include <linux/types.h>

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

/* Accepts a copied, naturally aligned envelope, never a userspace pointer.
 * On failure the output is zeroed. Does not dereference param_in/param_out;
 * future callers must still use access_ok/copy_from_user/copy_to_user.
 */
int sgx545_bridge32_decode(const struct sgx545_bridge32 *wire,
			 unsigned int copied_size,
			 struct sgx545_bridge_request *request);

#endif
