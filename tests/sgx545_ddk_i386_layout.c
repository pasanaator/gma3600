// SPDX-License-Identifier: GPL-2.0-only
/* Compile-only against actual pinned public DDK headers, in real -m32 mode. */
#include "pvr_bridge.h"
#include "sgx_bridge.h"
#include "sgxconfig.h"
#define SGX545_FREESTANDING
#include "sgx545_abi.h"

_Static_assert(sizeof(void *) == 4, "genuine i386 compilation");
_Static_assert(PVRSRV_DEVICE_TYPE_UNKNOWN == SGX545_DEVICE_TYPE_UNKNOWN, "index selector");
_Static_assert(PVRSRV_DEVICE_TYPE_SGX == SGX545_DEVICE_TYPE, "SGX device type");
_Static_assert(SRV_FLAGS_PERSIST == SGX545_CONNECT_PDUMP_PERSIST, "PDump persistence flag");
_Static_assert(SRV_FLAGS_PDUMP_ACTIVE == SGX545_CONNECT_PDUMP_ACTIVE, "PDump active flag");
_Static_assert(sizeof(PVRSRV_BRIDGE_PACKAGE) == sizeof(struct sgx545_bridge32), "envelope");
#ifdef SGX545_ORIGINAL_INTEL
_Static_assert(sizeof(PVRSRV_BRIDGE_OUT_CONNECT_SERVICES) == 8, "original Intel CONNECT size");
_Static_assert(sizeof(PVRSRV_BRIDGE_OUT_ENUMDEVICE) == sizeof(struct sgx545_intel_enumerate_out32), "original Intel enumeration reply");
_Static_assert(sizeof(PVRSRV_DEVICE_IDENTIFIER) == sizeof(struct sgx545_intel_device_id32), "original Intel device identifier");
_Static_assert(__builtin_offsetof(PVRSRV_DEVICE_IDENTIFIER, pszPDumpDevName) == 12, "original Intel PDump device slot");
_Static_assert(__builtin_offsetof(PVRSRV_DEVICE_IDENTIFIER, pszPDumpRegName) == 16, "original Intel PDump register slot");
#else
_Static_assert(sizeof(PVRSRV_BRIDGE_OUT_CONNECT_SERVICES) == 12, "compatibility CONNECT size");
_Static_assert(sizeof(PVRSRV_BRIDGE_OUT_ENUMDEVICE) == sizeof(struct sgx545_enumerate_out), "compatibility enumeration reply");
_Static_assert(sizeof(PVRSRV_DEVICE_IDENTIFIER) == sizeof(struct sgx545_device_id), "compatibility device identifier");
#endif
_Static_assert(PVRSRV_MAX_DEVICES == SGX545_MAX_DEVICES, "enumeration device count");
_Static_assert(__builtin_offsetof(PVRSRV_DEVICE_IDENTIFIER, eDeviceType) == 0, "identifier type");
_Static_assert(__builtin_offsetof(PVRSRV_DEVICE_IDENTIFIER, eDeviceClass) == 4, "identifier class");
_Static_assert(__builtin_offsetof(PVRSRV_DEVICE_IDENTIFIER, ui32DeviceIndex) == 8, "identifier index");
_Static_assert(__builtin_offsetof(PVRSRV_BRIDGE_OUT_ENUMDEVICE, asDeviceIdentifier) == 8, "enumeration array");
_Static_assert(sizeof(PVRSRV_BRIDGE_IN_CONNECT_SERVICES) == 8, "connect request");
_Static_assert(sizeof(PVRSRV_BRIDGE_IN_ACQUIRE_DEVICEINFO) == 12, "acquire request");
_Static_assert(__builtin_offsetof(PVRSRV_BRIDGE_IN_ACQUIRE_DEVICEINFO, uiDevIndex) == 4, "acquire index field");
_Static_assert(__builtin_offsetof(PVRSRV_BRIDGE_IN_ACQUIRE_DEVICEINFO, eDeviceType) == 8, "acquire type field");
_Static_assert(sizeof(PVRSRV_BRIDGE_OUT_ACQUIRE_DEVICEINFO) == sizeof(struct sgx545_handle_out), "acquire reply");
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
_Static_assert(sizeof(PVRSRV_BRIDGE_IN_ALLOC_SHARED_SYS_MEM) == 12, "shared allocation request");
_Static_assert(sizeof(PVRSRV_BRIDGE_IN_FREE_SHARED_SYS_MEM) == 56, "shared free request");
_Static_assert(sizeof(PVRSRV_BRIDGE_OUT_FREE_SHARED_SYS_MEM) == 4, "shared free reply");
_Static_assert(sizeof(PVRSRV_BRIDGE_OUT_MHANDLE_TO_MMAP_DATA) == sizeof(struct sgx545_map_info_out), "map metadata reply");
_Static_assert(sizeof(PVRSRV_BRIDGE_IN_MHANDLE_TO_MMAP_DATA) == 8, "map metadata request");
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
#ifdef SGX545_ORIGINAL_INTEL
_Static_assert(sizeof(PVRSRV_BRIDGE_IN_FREEDEVICEMEM) == 60, "original Intel device free request");
_Static_assert(sizeof(PVRSRV_BRIDGE_RETURN) == 8, "original Intel device free reply");
#else
_Static_assert(sizeof(PVRSRV_BRIDGE_IN_FREEDEVICEMEM) == 12, "compatibility device free request");
_Static_assert(sizeof(PVRSRV_BRIDGE_OUT_FREEDEVICEMEM) == 4, "device free reply");
#endif
_Static_assert(PVRSRV_BRIDGE_ALLOC_DEVICEMEM == SGX545_COMMAND(SGX545_CMD_ALLOC_DEVICE), "device alloc command");
_Static_assert(PVRSRV_BRIDGE_FREE_DEVICEMEM == SGX545_COMMAND(SGX545_CMD_FREE_DEVICE), "device free command");
_Static_assert((PVRSRV_MEM_READ | PVRSRV_MEM_WRITE | PVRSRV_MEM_NO_SYNCOBJ | PVRSRV_HAP_WRITECOMBINE) == SGX545_DEVICE_FLAGS, "device flags");
_Static_assert(PVRSRV_MEM_RAM_BACKED_ALLOCATION == SGX545_RAM_BACKED, "RAM backed flag");

_Static_assert(__builtin_offsetof(PVRSRV_BRIDGE_IN_ALLOCDEVICEMEM, hDevCookie) == 4, "device cookie offset");
_Static_assert(__builtin_offsetof(PVRSRV_BRIDGE_IN_ALLOCDEVICEMEM, hDevMemHeap) == 8, "device heap offset");
_Static_assert(__builtin_offsetof(PVRSRV_BRIDGE_IN_ALLOCDEVICEMEM, ui32Attribs) == 12, "device flags offset");
#ifdef SGX545_ORIGINAL_INTEL
#define uSize ui32Size
#define uAlignment ui32Alignment
#endif
_Static_assert(__builtin_offsetof(PVRSRV_BRIDGE_IN_ALLOCDEVICEMEM, uSize) == 16, "device size offset");
_Static_assert(__builtin_offsetof(PVRSRV_BRIDGE_IN_ALLOCDEVICEMEM, uAlignment) == 20, "device alignment offset");
_Static_assert(__builtin_offsetof(PVRSRV_BRIDGE_OUT_ALLOCDEVICEMEM, psKernelMemInfo) == __builtin_offsetof(struct sgx545_alloc_device_out, kernel_mem), "device token offset");
_Static_assert(__builtin_offsetof(PVRSRV_BRIDGE_OUT_ALLOCDEVICEMEM, sClientMemInfo) == __builtin_offsetof(struct sgx545_alloc_device_out, mem), "device client offset");
_Static_assert(__builtin_offsetof(PVRSRV_BRIDGE_OUT_ALLOCDEVICEMEM, sClientSyncInfo) == __builtin_offsetof(struct sgx545_alloc_device_out, sync), "device sync offset");

/* Intel's verified 20120717 release enables HWPERF and recovery. */
_Static_assert(sizeof(PVRSRV_BRIDGE_IN_SGXDEVINITPART2) == 2068, "Intel i386 initializer request");
_Static_assert(sizeof(PVRSRV_BRIDGE_OUT_SGXDEVINITPART2) == 8, "Intel i386 initializer reply");
_Static_assert(sizeof(SGX_BRIDGE_INIT_INFO) == 2060, "Intel i386 initializer body");
_Static_assert(__builtin_offsetof(PVRSRV_BRIDGE_IN_SGXDEVINITPART2, sInitInfo) == 8, "initializer body offset");
_Static_assert(__builtin_offsetof(SGX_BRIDGE_INIT_INFO, sScripts) == 68, "initializer scripts offset");
_Static_assert(__builtin_offsetof(SGX_BRIDGE_INIT_INFO, ui32ClientBuildOptions) == 1796, "initializer options offset");
_Static_assert(__builtin_offsetof(SGX_BRIDGE_INIT_INFO, sSGXStructSizes) == 1800, "initializer sizes offset");
_Static_assert(__builtin_offsetof(SGX_BRIDGE_INIT_INFO, asInitMemHandles) == 1988, "initializer handles offset");
_Static_assert(sizeof(SGX_INIT_COMMAND) == 12 && sizeof(SGX_INIT_SCRIPTS) == 1728, "Intel script layout");
_Static_assert(SGX_INIT_OP_WRITE_HW_REG == 1 && SGX_INIT_OP_HALT == 2, "Intel script opcodes");

_Static_assert(sizeof(struct sgx545_init2_in32) == sizeof(PVRSRV_BRIDGE_IN_SGXDEVINITPART2), "project initializer wire request");
_Static_assert(sizeof(struct sgx545_init2_out32) == sizeof(PVRSRV_BRIDGE_OUT_SGXDEVINITPART2), "project initializer wire reply");
#define INIT_FIELD(ddk, wire) _Static_assert(__builtin_offsetof(SGX_BRIDGE_INIT_INFO, ddk) == \
    __builtin_offsetof(struct sgx545_init2_info32, wire), "initializer field " #ddk)
INIT_FIELD(hKernelCCBMemInfo, ccb);
INIT_FIELD(hKernelSGXHostCtlMemInfo, host_control);
INIT_FIELD(hKernelSGXMiscMemInfo, misc);
INIT_FIELD(aui32HostKickAddr, host_kick);
INIT_FIELD(sScripts, scripts);
INIT_FIELD(ui32ClientBuildOptions, build_options);
INIT_FIELD(sSGXStructSizes, struct_sizes);
INIT_FIELD(hKernelHWPerfCBMemInfo, hwperf);
INIT_FIELD(hKernelTASigBufferMemInfo, ta_signature);
INIT_FIELD(hKernel3DSigBufferMemInfo, signature_3d);
INIT_FIELD(ui32EDMTaskReg0, edm_task);
INIT_FIELD(ui32ClkGateCtl, clock_control);
INIT_FIELD(ui32ClkGateStatusReg, clock_status_reg);
INIT_FIELD(ui32ClkGateStatusMask, clock_status_mask);
INIT_FIELD(ui32CacheControl, cache_control);
INIT_FIELD(asInitDevData, dev_data);
INIT_FIELD(asInitMemHandles, mem_handles);
