/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA3600_SGX_PREPARE_H
#define GMA3600_SGX_PREPARE_H
/* Included after services_headers.h; this is internal kernel glue only. */
struct _SGX_DEVICE_MAP_;
PVRSRV_ERROR gma3600_sgx_borrow_map(struct _SGX_DEVICE_MAP_ *map);
void gma3600_sgx_blocked_write(IMG_UINT32 offset, IMG_UINT32 value);
#endif
