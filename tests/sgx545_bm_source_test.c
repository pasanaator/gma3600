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
struct fake_mapping { unsigned address, flags, count; IMG_SYS_PHYADDR *scatter; };
typedef struct _MMU_HEAP_ { unsigned capacity; struct fake_mapping slots[8]; } MMU_HEAP;
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
static unsigned allocations, areas_live, calls, fail_alloc, fail_register, fail_va, bad_getter, bad_pte, bad_free, verifies, retires;
static MMU_HEAP mmu;
struct fake_area { void *linear; IMG_SYS_PHYADDR *scatter; int live; };
static struct fake_area areas[8];
static unsigned expected_alignment = 4096;
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
    unsigned i;
    assert(scatter && linear == backing && bytes == 16384);
    assert((flags & PVRSRV_HAP_MAPTYPE_MASK) == PVRSRV_HAP_KERNEL_ONLY);
    assert((flags & PVRSRV_HAP_CACHETYPE_MASK) == PVRSRV_HAP_WRITECOMBINE);
    if (fail_register) return PVRSRV_ERROR_BAD_MAPPING;
    for (i = 0; i < 8 && areas[i].live; i++) {}
    assert(i < 8); areas[i] = (struct fake_area){linear, scatter, 1};
    *handle = &areas[i]; areas_live++; return PVRSRV_OK;
}
static PVRSRV_ERROR OSUnRegisterDiscontigMem(void *linear, IMG_SIZE_T bytes, unsigned flags, void *handle)
{
    struct fake_area *area = handle;
    assert(area >= areas && area < areas + 8 && area->live && area->linear == linear && linear == backing && bytes == 16384);
    assert((flags & PVRSRV_HAP_MAPTYPE_MASK) == PVRSRV_HAP_KERNEL_ONLY);
    area->live = 0; areas_live--; return PVRSRV_OK;
}
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
static struct fake_mapping *find_mapping(unsigned address)
{
    unsigned i;
    for (i = 0; i < 8; i++) if (mmu.slots[i].address && address >= mmu.slots[i].address && address < mmu.slots[i].address + 16384) return &mmu.slots[i];
    assert(!"missing live native mapping"); return NULL;
}
static IMG_BOOL allocate_va(void *heap, IMG_SIZE_T bytes, IMG_SIZE_T *actual, unsigned flags, unsigned alignment, IMG_DEV_VIRTADDR *address)
{
    unsigned i;
    assert(heap == &mmu && bytes == 16384 && !actual && !flags && alignment == expected_alignment);
    if (fail_va) return IMG_FALSE;
    for (i = 0; i < 8 && mmu.slots[i].address; i++) {}
    assert(i < 8); address->uiAddr = (0xf4010000 + i * 0x100000 + alignment - 1) & ~(alignment - 1);
    mmu.slots[i].address = address->uiAddr; return IMG_TRUE;
}
static void map_scatter(void *heap, IMG_DEV_VIRTADDR address, IMG_SYS_PHYADDR *scatter, IMG_SIZE_T bytes, unsigned flags, void *tag)
{
    struct fake_mapping *m = find_mapping(address.uiAddr);
    assert(heap == &mmu && bytes == 16384 && areas_live && tag);
    m->scatter = scatter; m->count = bytes / 4096; m->flags = flags;
}
static IMG_DEV_PHYADDR physical_page(void *heap, IMG_DEV_VIRTADDR address)
{
    struct fake_mapping *m = find_mapping(address.uiAddr);
    assert(heap == &mmu);
    return (IMG_DEV_PHYADDR){m->scatter[(address.uiAddr - m->address) / 4096].uiAddr ^ (bad_getter ? 4096U : 0U)};
}
static void free_va(void *heap, IMG_DEV_VIRTADDR address, unsigned bytes)
{
    unsigned i; struct fake_mapping *m = find_mapping(address.uiAddr);
    assert(heap == &mmu && bytes == 16384);
    for (i = 0; i < 8; i++) assert(!areas[i].live || areas[i].scatter != m->scatter);
    if (!bad_free) memset(m, 0, sizeof(*m));
}
int gma3600_sgx_mmu_checkpoint(void *heap, __u32 *capacity, __u32 *tables)
{ assert(heap == &mmu); *capacity = mmu.capacity; *tables = 1; return 0; }
int gma3600_sgx_mmu_verify(struct gma_sgx_mmu_binding *binding, const __u32 *physical, __u32 count, __u32 *tables)
{
    unsigned i; struct fake_mapping *m = find_mapping(binding->address);
    assert(binding->heap == &mmu && count == 4 && areas_live);
    assert((m->flags & SGX545_DEVICE_MMU_FLAGS) == binding->memory_flags);
    assert(!(m->flags & PVRSRV_MEM_RAM_BACKED_ALLOCATION));
    for (i = 0; i < count; i++) assert(m->scatter[i].uiAddr == physical[i]);
    verifies++; *tables = 1; return bad_pte ? -EIO : 0;
}
int gma3600_sgx_mmu_retired(void *heap, __u32 address, __u32 bytes, __u32 capacity, __u32 *tables)
{
    unsigned i;
    assert(heap == &mmu && capacity == mmu.capacity && bytes == 16384);
    retires++; *tables = 1;
    for (i = 0; address && i < 8; i++) if (mmu.slots[i].address == address) return -EUCLEAN;
    return 0;
}
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
        assert(!allocations && !areas_live);
        if (!result) assert(out.passes == 2 && out.alias_address == 0xf4010000 && verifies == 2 && retires == 2);
        for (i = 0; i < 16384; i++) assert(backing[i] == (unsigned char)(i * 17));
    }
    fail_alloc = fail_register = fail_va = bad_pte = bad_free = bad_getter = 0;
    memset(&mmu, 0, sizeof(mmu)); mmu.capacity = 100;
    for (mode = 1; mode <= 4; mode++) {
        void *owner = NULL; struct gma_sgx_mmu_binding binding = {0};
        calls = 0; fail_alloc = mode;
        assert(gma3600_sgx_bm_allocate(&heap, pages, 4, backing, 16380,
            SGX545_INTEL_INIT_FLAGS | SGX545_HEAP_FLAGS, 4096, &owner, &binding) == -ENOMEM);
        assert(!owner && !binding.live && !allocations && !areas_live);
    }
    fail_alloc = 0;
    {
        void *first = NULL, *second = NULL, *failed = NULL;
        struct gma_sgx_mmu_binding a = {0}, b = {0}, failed_binding = {0};
        PVRSRV_KERNEL_MEM_INFO info = {0}; unsigned tables;
        unsigned flags = SGX545_INTEL_INIT_FLAGS | SGX545_HEAP_FLAGS;
        unsigned *temporary = malloc(sizeof(pages)); assert(temporary);
        memcpy(temporary, pages, sizeof(pages));
        expected_alignment = 65536;
        assert(!gma3600_sgx_bm_allocate(&heap, temporary, 4, backing, 16380, flags, 65536, &first, &a));
        memset(temporary, 0, sizeof(pages)); free(temporary); /* Callback input cannot escape. */
        assert(first && !(a.address & 65535) && allocations == 4 && areas_live == 1);
        expected_alignment = 4096; fail_va = 1;
        assert(gma3600_sgx_bm_allocate(&heap, pages, 4, backing, 16380, flags, 4096, &failed, &failed_binding) == -ENOMEM);
        assert(!failed && !failed_binding.live && areas_live == 1 && allocations == 4);
        fail_va = 0;
        assert(!gma3600_sgx_bm_allocate(&heap, pages, 4, backing, 16380, flags, 4096, &second, &b));
        assert(allocations == 8 && areas_live == 2 && a.address != b.address);
        info.pvLinAddrKM = backing; info.sDevVAddr.uiAddr = a.address; info.ui32Flags = flags; info.uAllocSize = 16380;
        assert(!gma3600_sgx_bm_info(first, &info, 1) && !gma3600_sgx_bm_info(first, &info, 0));
        info.sMemBlk.hOSMemHandle = (void *)1;
        assert(gma3600_sgx_bm_info(first, &info, 0) == -EIO);
        assert(!gma3600_sgx_bm_info(first, &info, 1));
        assert(!gma3600_sgx_bm_verify(first, pages, 4, &tables) && !gma3600_sgx_bm_verify(second, pages, 4, &tables));
        assert(!gma3600_sgx_bm_release(&first) && !first && allocations == 4 && areas_live == 1);
        assert(!gma3600_sgx_bm_verify(second, pages, 4, &tables));
        backing[20] ^= 1; backing[20] ^= 1; /* CPU ownership survives native free. */
        assert(!gma3600_sgx_bm_release(&second) && !second && !allocations && !areas_live);
        assert(gma3600_sgx_bm_release(&first) == -ENOENT);
        assert(!gma3600_sgx_bm_allocate(&heap, pages, 4, backing, 16380, flags, 4096, &first, &a));
        bad_free = 1;
        assert(gma3600_sgx_bm_release(&first) == -EUCLEAN && first && allocations == 2 && !areas_live);
        assert(gma3600_sgx_bm_release(&first) == -EIO); /* No repeated native free. */
        /* Test-only recovery of quarantined bookkeeping after simulated PTE retirement. */
        bad_free = 0; memset(mmu.slots, 0, sizeof(mmu.slots));
        free_(PVRSRV_OS_PAGEABLE_HEAP, 4 * sizeof(IMG_SYS_PHYADDR), ((struct gma_sgx_bm_owner *)first)->scatter);
        free_(PVRSRV_OS_PAGEABLE_HEAP, sizeof(struct gma_sgx_bm_owner), first);
        assert(!allocations);
    }
    free(backing);
    puts("Native BM source tests passed: retained allocation, 64K alignment, BM/OS snapshot attachment, sibling/free/quarantine lifetime; actual wrap/map/free/getters, two VA reuse cycles, native flags, OS registration order, allocation/map failures and backing preservation");
    return 0;
}
