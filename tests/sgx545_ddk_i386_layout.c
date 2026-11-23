// SPDX-License-Identifier: GPL-2.0-only
/* Compile-only against actual pinned public DDK headers, in real -m32 mode. */
#include "pvr_bridge.h"
#include "sgx_bridge.h"
#include "sgxconfig.h"
#define SGX545_FREESTANDING
#include "sgx545_abi.h"

_Static_assert(sizeof(void *) == 4, "genuine i386 compilation");
_Static_assert(sizeof(PVRSRV_BRIDGE_PACKAGE) == sizeof(struct sgx545_bridge32), "envelope");
_Static_assert(sizeof(PVRSRV_BRIDGE_OUT_CONNECT_SERVICES) == 12, "CONNECT size");
_Static_assert(__builtin_offsetof(PVRSRV_BRIDGE_OUT_CONNECT_SERVICES, hKernelServices) == 4, "CONNECT alignment");
_Static_assert(sizeof(PVRSRV_BRIDGE_IN_INITSRV_DISCONNECT) == 8, "abort request");
_Static_assert(sizeof(PVRSRV_BRIDGE_IN_SGXINFO_FOR_SRVINIT) == 8, "init info request");
_Static_assert(sizeof(PVRSRV_BRIDGE_OUT_SGXINFO_FOR_SRVINIT) == sizeof(struct sgx545_init_info_out), "init info reply");
_Static_assert(sizeof(PVRSRV_HEAP_INFO) == sizeof(struct sgx545_heap32), "heap layout");
_Static_assert(PVRSRV_MAX_CLIENT_HEAPS == SGX545_MAX_HEAPS, "heap count");
#define FIELD(ddk, wire) _Static_assert(__builtin_offsetof(PVRSRV_HEAP_INFO, ddk) == \
    __builtin_offsetof(struct sgx545_heap32, wire), "heap field " #ddk)
FIELD(ui32HeapID, id);
FIELD(hDevMemHeap, handle);
FIELD(sDevVAddrBase, base);
FIELD(ui32HeapByteSize, size);
FIELD(ui32Attribs, attributes);
FIELD(ui32XTileStride, tile_stride);
_Static_assert(PVRSRV_BRIDGE_INITSRV_CONNECT == SGX545_COMMAND(SGX545_CMD_INIT_CONNECT), "init connect command");
_Static_assert(PVRSRV_BRIDGE_INITSRV_DISCONNECT == SGX545_COMMAND(SGX545_CMD_INIT_DISCONNECT), "init disconnect command");
_Static_assert(PVRSRV_BRIDGE_SGXINFO_FOR_SRVINIT == SGX545_COMMAND(SGX545_CMD_INIT_INFO), "init info command");
_Static_assert(SGX_SHARED_3DPARAMETERS_HEAP_SIZE == 0, "per-context profile disables shared PB");
_Static_assert(SGX_PERCONTEXT_3DPARAMETERS_HEAP_SIZE > 0, "per-context PB remains active");
