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
_Static_assert(sizeof(PVRSRV_CLIENT_MEM_INFO) == sizeof(struct sgx545_client_mem32), "client memory record");
_Static_assert(sizeof(PVRSRV_BRIDGE_OUT_ALLOC_SHARED_SYS_MEM) == sizeof(struct sgx545_alloc_shared_out), "shared allocation reply");
_Static_assert(sizeof(PVRSRV_BRIDGE_IN_FREE_SHARED_SYS_MEM) == 56, "shared free request");
_Static_assert(sizeof(PVRSRV_BRIDGE_OUT_FREE_SHARED_SYS_MEM) == 4, "shared free reply");
_Static_assert(sizeof(PVRSRV_BRIDGE_OUT_MHANDLE_TO_MMAP_DATA) == sizeof(struct sgx545_map_info_out), "map metadata reply");
_Static_assert(PVRSRV_BRIDGE_ALLOC_SHARED_SYS_MEM == SGX545_COMMAND(SGX545_CMD_ALLOC_SHARED), "shared alloc command");
_Static_assert(PVRSRV_BRIDGE_FREE_SHARED_SYS_MEM == SGX545_COMMAND(SGX545_CMD_FREE_SHARED), "shared free command");
_Static_assert(PVRSRV_BRIDGE_MHANDLE_TO_MMAP_DATA == SGX545_COMMAND(SGX545_CMD_MAP_INFO), "map metadata command");
#define MEM_FIELD(ddk, wire) _Static_assert(__builtin_offsetof(PVRSRV_CLIENT_MEM_INFO, ddk) == \
    __builtin_offsetof(struct sgx545_client_mem32, wire), "memory field " #ddk)
MEM_FIELD(pvLinAddr, linear);
MEM_FIELD(pvLinAddrKM, kernel_linear);
MEM_FIELD(sDevVAddr, device_address);
MEM_FIELD(sCpuPAddr, physical_address);
MEM_FIELD(ui32Flags, flags);
MEM_FIELD(ui32ClientFlags, client_flags);
MEM_FIELD(uAllocSize, alloc_size);
MEM_FIELD(psClientSyncInfo, sync_info);
MEM_FIELD(hMappingInfo, mapping);
MEM_FIELD(hKernelMemInfo, kernel_mem);
MEM_FIELD(hResItem, resource);
MEM_FIELD(psNext, next);

_Static_assert(sizeof(PVRSRV_BRIDGE_IN_ALLOCDEVICEMEM) == 24, "device allocation request");
_Static_assert(sizeof(PVRSRV_BRIDGE_OUT_ALLOCDEVICEMEM) == sizeof(struct sgx545_alloc_device_out), "device allocation reply");
_Static_assert(sizeof(PVRSRV_BRIDGE_IN_FREEDEVICEMEM) == 12, "device free request");
_Static_assert(sizeof(PVRSRV_BRIDGE_OUT_FREEDEVICEMEM) == 4, "device free reply");
_Static_assert(PVRSRV_BRIDGE_ALLOC_DEVICEMEM == SGX545_COMMAND(SGX545_CMD_ALLOC_DEVICE), "device alloc command");
_Static_assert(PVRSRV_BRIDGE_FREE_DEVICEMEM == SGX545_COMMAND(SGX545_CMD_FREE_DEVICE), "device free command");
_Static_assert((PVRSRV_MEM_READ | PVRSRV_MEM_WRITE | PVRSRV_MEM_NO_SYNCOBJ | PVRSRV_HAP_WRITECOMBINE) == SGX545_DEVICE_FLAGS, "device flags");
_Static_assert(PVRSRV_MEM_RAM_BACKED_ALLOCATION == SGX545_RAM_BACKED, "RAM backed flag");

_Static_assert(__builtin_offsetof(PVRSRV_BRIDGE_IN_ALLOCDEVICEMEM, hDevCookie) == 4, "device cookie offset");
_Static_assert(__builtin_offsetof(PVRSRV_BRIDGE_IN_ALLOCDEVICEMEM, hDevMemHeap) == 8, "device heap offset");
_Static_assert(__builtin_offsetof(PVRSRV_BRIDGE_IN_ALLOCDEVICEMEM, ui32Attribs) == 12, "device flags offset");
_Static_assert(__builtin_offsetof(PVRSRV_BRIDGE_IN_ALLOCDEVICEMEM, uSize) == 16, "device size offset");
_Static_assert(__builtin_offsetof(PVRSRV_BRIDGE_IN_ALLOCDEVICEMEM, uAlignment) == 20, "device alignment offset");
_Static_assert(__builtin_offsetof(PVRSRV_BRIDGE_OUT_ALLOCDEVICEMEM, psKernelMemInfo) == __builtin_offsetof(struct sgx545_alloc_device_out, kernel_mem), "device token offset");
_Static_assert(__builtin_offsetof(PVRSRV_BRIDGE_OUT_ALLOCDEVICEMEM, sClientMemInfo) == __builtin_offsetof(struct sgx545_alloc_device_out, mem), "device client offset");
_Static_assert(__builtin_offsetof(PVRSRV_BRIDGE_OUT_ALLOCDEVICEMEM, sClientSyncInfo) == __builtin_offsetof(struct sgx545_alloc_device_out, sync), "device sync offset");
