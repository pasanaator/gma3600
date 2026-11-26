// SPDX-License-Identifier: GPL-2.0-only
/* Real native BM bodies/types; OS registration and device/MMU callbacks mocked. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pvr_bridge.h"
#include "sgx_bridge.h"
#include "sgx_options.h"
#define SGX545_FREESTANDING
#include "gma3600_sgx_bm.h"
#include "gma3600_sgx_mmu.h"
#include "os_seam.h"
typedef struct _DEVICE_MEMORY_HEAP_INFO_ DEVICE_MEMORY_HEAP_INFO;
typedef struct _RA_ARENA_ RA_ARENA;
typedef struct _HASH_TABLE_ HASH_TABLE;
typedef struct _PVRSRV_PER_PROCESS_DATA_ PVRSRV_PER_PROCESS_DATA;
typedef struct _BM_CONTEXT_ BM_CONTEXT;
typedef struct _BM_MAPPING_ BM_MAPPING;
typedef struct _MMU_CONTEXT_ MMU_CONTEXT;
typedef struct _MMU_HEAP_ { unsigned address, flags, count, capacity; IMG_SYS_PHYADDR *scatter; } MMU_HEAP;
typedef struct { unsigned ui32DataPageSize; } DEV_ARENA_DESCRIPTOR;
typedef struct _PDUMP_MMU_ATTRIB_ PDUMP_MMU_ATTRIB;
typedef struct _PVRSRV_DEVICE_NODE_ PVRSRV_DEVICE_NODE;
struct _PVRSRV_DEVICE_NODE_ {
    IMG_BOOL (*pfnMMUAlloc)(void *, IMG_SIZE_T, IMG_SIZE_T *, unsigned, unsigned, IMG_DEV_VIRTADDR *);
    void (*pfnMMUMapScatter)(void *, IMG_DEV_VIRTADDR, IMG_SYS_PHYADDR *, IMG_SIZE_T, unsigned, void *);
    void (*pfnMMUMapPages)(); void (*pfnMMUMapPagesSparse)();
    void (*pfnMMUMapShadow)(); void (*pfnMMUMapShadowSparse)();
    IMG_DEV_PHYADDR (*pfnMMUGetPhysPageAddr)(void *, IMG_DEV_VIRTADDR);
    void (*pfnMMUFree)(void *, IMG_DEV_VIRTADDR, unsigned);
    void (*pfnCacheInvalidate)(PVRSRV_DEVICE_NODE *);
};
#include "buffer_manager.h"
typedef int SYS_DATA;
static unsigned allocations, areas, calls, fail_alloc, fail_register, fail_va, bad_getter, bad_pte, bad_free, verifies, retires;
static MMU_HEAP mmu;
static unsigned char *backing;
static int unexpected(void) { assert(!"unexpected ownership/OS path"); return 0; }
#define PVR_DPF(args) ((void)0)
#define PVR_ASSERT assert
#define HOST_PAGESIZE() 4096U
#define HOST_PAGEALIGN(n) (((n) + 4095U) & ~4095U)
#define OSMemSet memset
static PVRSRV_ERROR alloc_(unsigned heap, size_t bytes, void **out)
{
    assert(heap == PVRSRV_OS_PAGEABLE_HEAP);
    if (++calls == fail_alloc) return PVRSRV_ERROR_OUT_OF_MEMORY;
    *out = calloc(1, bytes); assert(*out); allocations++; return PVRSRV_OK;
}
static void free_(unsigned heap, size_t bytes, void *p)
{ assert(heap == PVRSRV_OS_PAGEABLE_HEAP && bytes && p && allocations); allocations--; free(p); }
#define OSAllocMem(heap, bytes, out, handle, text) alloc_(heap, bytes, (void **)(out))
#define OSFreeMem(heap, bytes, p, handle) free_(heap, bytes, p)
static void SysAcquireData(SYS_DATA **out) { static SYS_DATA data; *out = &data; }
static IMG_CPU_PHYADDR SysSysPAddrToCpuPAddr(IMG_SYS_PHYADDR p) { return (IMG_CPU_PHYADDR){p.uiAddr}; }
static IMG_SYS_PHYADDR SysCpuPAddrToSysPAddr(IMG_CPU_PHYADDR p) { return (IMG_SYS_PHYADDR){p.uiAddr}; }
static PVRSRV_ERROR OSRegisterDiscontigMem(IMG_SYS_PHYADDR *scatter, void *linear, IMG_SIZE_T bytes, unsigned flags, void **handle)
{
    assert(!areas && scatter && linear == backing && bytes == 16384);
    assert((flags & PVRSRV_HAP_MAPTYPE_MASK) == PVRSRV_HAP_KERNEL_ONLY);
    assert((flags & PVRSRV_HAP_CACHETYPE_MASK) == PVRSRV_HAP_WRITECOMBINE);
    if (fail_register) return PVRSRV_ERROR_BAD_MAPPING;
    *handle = &areas; areas++; return PVRSRV_OK;
}
static PVRSRV_ERROR OSUnRegisterDiscontigMem(void *linear, IMG_SIZE_T bytes, unsigned flags, void *handle)
{ assert(areas == 1 && handle == &areas && linear == backing && bytes == 16384 && (mmu.address || fail_va) && (flags & PVRSRV_HAP_MAPTYPE_MASK) == PVRSRV_HAP_KERNEL_ONLY); areas--; return PVRSRV_OK; }
#define OSRegisterMem(...) unexpected()
#define OSUnRegisterMem(...) unexpected()
#define OSReservePhys(...) unexpected()
#define OSReserveDiscontigPhys(...) unexpected()
#define OSUnReservePhys(...) unexpected()
#define OSUnReserveDiscontigPhys(...) unexpected()
#define OSGetSubMemHandle(...) unexpected()
#define OSReleaseSubMemHandle(...) unexpected()
#define HASH_Remove(table, key) ((void)(table), (void)(key), unexpected())
#define RA_Free(...) unexpected()
#define BM_FreeMemory(...) unexpected()
#define ZeroBuf(...) unexpected()
#define PVRSRVBMBufDecRef(p) (assert((p)->ui32RefCount == 1), (p)->ui32RefCount--)
#include "bm_functions.inc"
static IMG_BOOL allocate_va(void *heap, IMG_SIZE_T bytes, IMG_SIZE_T *actual, unsigned flags, unsigned alignment, IMG_DEV_VIRTADDR *address)
{ assert(heap == &mmu && !mmu.address && bytes == 16384 && !actual && !flags && alignment == 4096); if (fail_va) return IMG_FALSE; mmu.address = address->uiAddr = 0xf4010000; return IMG_TRUE; }
static void map_scatter(void *heap, IMG_DEV_VIRTADDR address, IMG_SYS_PHYADDR *scatter, IMG_SIZE_T bytes, unsigned flags, void *tag)
{ assert(heap == &mmu && address.uiAddr == mmu.address && bytes == 16384 && areas == 1 && tag); mmu.scatter = scatter; mmu.count = bytes / 4096; mmu.flags = flags; }
static IMG_DEV_PHYADDR physical_page(void *heap, IMG_DEV_VIRTADDR address)
{ assert(heap == &mmu && address.uiAddr >= mmu.address && address.uiAddr < mmu.address + 16384); return (IMG_DEV_PHYADDR){mmu.scatter[(address.uiAddr - mmu.address) / 4096].uiAddr ^ (bad_getter ? 4096U : 0U)}; }
static void free_va(void *heap, IMG_DEV_VIRTADDR address, unsigned bytes)
{ assert(heap == &mmu && address.uiAddr == mmu.address && bytes == 16384 && !areas); if (!bad_free) mmu.address = 0; }
int gma3600_sgx_mmu_checkpoint(void *heap, __u32 *capacity, __u32 *tables)
{ assert(heap == &mmu); *capacity = mmu.capacity; *tables = 1; return 0; }
int gma3600_sgx_mmu_verify(struct gma_sgx_mmu_binding *binding, const __u32 *physical, __u32 count, __u32 *tables)
{
    unsigned i; assert(binding->heap == &mmu && count == 4 && binding->address == mmu.address && areas);
    assert((mmu.flags & SGX545_DEVICE_MMU_FLAGS) == binding->memory_flags);
    assert(!(mmu.flags & PVRSRV_MEM_RAM_BACKED_ALLOCATION));
    for (i = 0; i < count; i++) assert(mmu.scatter[i].uiAddr == physical[i]);
    verifies++; *tables = 1; return bad_pte ? -EIO : 0;
}
int gma3600_sgx_mmu_retired(void *heap, __u32 address, __u32 bytes, __u32 capacity, __u32 *tables)
{ assert(heap == &mmu && capacity == mmu.capacity && !areas && bytes == 16384 && (!address || address == 0xf4010000)); retires++; *tables = 1; return mmu.address ? -EUCLEAN : 0; }
int main(void)
{
    PVRSRV_DEVICE_NODE node = {.pfnMMUAlloc = allocate_va, .pfnMMUMapScatter = map_scatter,
        .pfnMMUGetPhysPageAddr = physical_page, .pfnMMUFree = free_va};
    BM_CONTEXT context = {.psDeviceNode = &node};
    BM_HEAP heap = {.pBMContext = &context, .pMMUHeap = &mmu};
    const unsigned pages[] = {0x1000, 0x9000, 0x3000, 0x7000};
    unsigned i, mode;
    assert(!posix_memalign((void **)&backing, 4096, 16384));
    for (i = 0; i < 16384; i++) backing[i] = i * 17;
    for (mode = 0; mode < 14; mode++) {
        struct sgx545_bm_check out = {0};
        int result;
        unsigned flags = SGX545_INTEL_INIT_FLAGS | SGX545_HEAP_FLAGS;
        if (mode == 11) flags = (flags & ~SGX545_DEVICE_ACCESS) | SGX545_DEVICE_READ;
        if (mode == 12) flags = (flags & ~SGX545_DEVICE_ACCESS) | SGX545_DEVICE_WRITE;
        if (mode == 13) flags = SGX545_DEVICE_FLAGS | SGX545_HEAP_FLAGS;
        memset(&mmu, 0, sizeof(mmu)); mmu.capacity = 100;
        calls = verifies = retires = 0; fail_alloc = mode >= 1 && mode <= 5 ? mode : 0;
        fail_register = mode == 6; fail_va = mode == 7; bad_pte = mode == 8; bad_free = mode == 9; bad_getter = mode == 10;
        result = gma3600_sgx_bm_cycle(&heap, pages, 4, backing, 16380,
            flags, &out);
        assert(result == (fail_alloc || fail_register || fail_va ? -ENOMEM : (bad_pte || bad_getter) ? -EIO : bad_free ? -EUCLEAN : 0));
        assert(!allocations && !areas);
        if (!result) assert(out.passes == 2 && out.alias_address == 0xf4010000 && verifies == 2 && retires == 2);
        for (i = 0; i < 16384; i++) assert(backing[i] == (unsigned char)(i * 17));
    }
    free(backing);
    puts("Native BM source tests passed: actual wrap/map/free/getters, two VA reuse cycles, native flags, OS registration order, allocation/map failures and backing preservation");
    return 0;
}
