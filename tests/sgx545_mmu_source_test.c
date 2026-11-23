// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "sgxdefs.h"
#include "sgxmmu.h"
#include "gma3600_sgx_mmu.h"
enum {DEVICE_MEMORY_HEAP_SHARED, DEVICE_MEMORY_HEAP_SHARED_EXPORTED, DEVICE_MEMORY_HEAP_PERCONTEXT, DEVICE_MEMORY_HEAP_KERNEL};
typedef struct { void *psLocalDevMemArena; } DEVICE_MEMORY_HEAP_INFO;
typedef struct {
    IMG_DEV_VIRTADDR BaseDevVAddr;
    IMG_UINT32 ui32Size, ui32HeapID;
    const char *pszName;
    int DevMemHeapType;
    DEVICE_MEMORY_HEAP_INFO *psDeviceMemoryHeapInfo;
} DEV_ARENA_DESCRIPTOR;
typedef struct { void *pvMMUContextList; unsigned ui32CacheControl; } PVRSRV_SGXDEV_INFO;
typedef struct { void *PTPageCpuVAddr, *hPTPageOSMemHandle; unsigned ui32ValidPTECount; } MMU_PT_INFO;
typedef struct _MMU_CONTEXT_ {
    MMU_PT_INFO *apsPTInfoList[1024];
    PVRSRV_SGXDEV_INFO *psDevInfo;
    void *pvPDCpuVAddr;
    struct _MMU_CONTEXT_ *psNext;
} MMU_CONTEXT;
typedef struct _MMU_HEAP_ MMU_HEAP;
typedef struct { IMG_UINTPTR_T start; IMG_SIZE_T size; bool live; } RESERVATION;
typedef struct { MMU_HEAP *heap; RESERVATION slots[8]; } RA_ARENA;
struct _MMU_HEAP_ {
    MMU_CONTEXT *psMMUContext;
    DEV_ARENA_DESCRIPTOR *psDevArena;
    RA_ARENA *psVMArena;
    unsigned ui32DataPageSize, ui32DataPageMask, ui32PDShift, ui32PTShift, ui32PTMask;
    unsigned ui32PDBaseIndex, ui32PageTableCount, ui32PTNumEntriesUsable, ui32PTETotalUsable;
    bool bHasSparseMappings;
};
typedef int SYS_DATA;
#define PVRSRV_MEM_READ 1
#define PVRSRV_MEM_WRITE 2
#define PVRSRV_MEM_USER_SUPPLIED_DEVVADDR 0x100
#define PVRSRV_MEM_CACHE_CONSISTENT 0x200
#define PVRSRV_MEM_EDM_PROTECT 0x400
#define PVRSRV_OS_PAGEABLE_HEAP 0
#define PVRSRV_DEVICE_TYPE_SGX 7
#define PT_DEBUG 0
#define PT_DUMP 0
#define HOST_PAGESIZE() 4096
#define PVR_DPF(args) ((void)0)
#define PVR_ASSERT(value) assert(value)
#define PDUMPCOMMENT(...) ((void)0)
#define PDUMPPDENTRIES(...) ((void)0)
#define MakeKernelPageReadWrite(p) ((void)(p))
#define MakeKernelPageReadOnly(p) ((void)(p))
static unsigned pt_pages, pt_metadata, reservations, fail_pt_at, pt_attempts, fail_reserve;
static void OSMemoryBarrier(void) { }
static void SysAcquireData(SYS_DATA **p) { static SYS_DATA data; *p = &data; }
static void CheckPT(MMU_PT_INFO *pt)
{
    unsigned i, valid = 0;
    assert(pt && pt->PTPageCpuVAddr);
    for (i = 0; i < 1024; i++) valid += !!(((unsigned *)pt->PTPageCpuVAddr)[i] & SGX_MMU_PTE_VALID);
    assert(valid == pt->ui32ValidPTECount);
}
static void MMU_InvalidateDirectoryCache(PVRSRV_SGXDEV_INFO *d) { d->ui32CacheControl |= 1; }
static void MMU_InvalidatePageTableCache(PVRSRV_SGXDEV_INFO *d) { d->ui32CacheControl |= 2; }
static IMG_DEV_PHYADDR SysSysPAddrToDevPAddr(unsigned type, IMG_SYS_PHYADDR p)
{ assert(type == 7); return (IMG_DEV_PHYADDR){p.uiAddr}; }
static void OSGetCurrentProcessNameKM(char *name, unsigned size) { memset(name, 0, size); }
static int OSGetCurrentProcessIDKM(void) __attribute__((unused));
static int OSGetCurrentProcessIDKM(void) { return 1; }
static void _FreePageTableMemory(MMU_HEAP *heap, MMU_PT_INFO *pt)
{ assert(pt_pages && pt->PTPageCpuVAddr); pt_pages--; free(pt->PTPageCpuVAddr); }
static void OSFreeMem(unsigned flags, size_t size, void *p, void *handle)
{ assert(pt_metadata && p && size == sizeof(MMU_PT_INFO)); pt_metadata--; free(p); }
static IMG_BOOL RA_Alloc(RA_ARENA *arena, size_t bytes, size_t *actual, void *priv,
    unsigned flags, unsigned alignment, unsigned offset, void *ref, unsigned ref_size, uintptr_t *address);
static void RA_Free(RA_ARENA *arena, uintptr_t address, int free_pages);
static IMG_BOOL _DeferredAllocPagetables(MMU_HEAP *heap, IMG_DEV_VIRTADDR address, unsigned bytes);

#include "mmu_functions.inc"

static IMG_BOOL RA_Alloc(RA_ARENA *arena, size_t bytes, size_t *actual, void *priv,
    unsigned flags, unsigned alignment, unsigned offset, void *ref, unsigned ref_size, uintptr_t *address)
{
    uintptr_t pos = arena->heap->psDevArena->BaseDevVAddr.uiAddr;
    unsigned i, slot;
    bool overlap;
    if (fail_reserve) { fail_reserve--; return false; }
    do {
        pos = (pos + alignment - 1) & ~(uintptr_t)(alignment - 1); overlap = false;
        for (i = 0; i < 8; i++) {
            RESERVATION *r = &arena->slots[i];
            if (r->live && pos < r->start + r->size && pos + bytes > r->start) {
                pos = r->start + r->size; overlap = true; break;
            }
        }
    } while (overlap);
    if (pos + bytes > (uint64_t)arena->heap->psDevArena->BaseDevVAddr.uiAddr + arena->heap->psDevArena->ui32Size)
        return false;
    for (slot = 0; slot < 8; slot++) if (!arena->slots[slot].live) break;
    assert(slot < 8);
    arena->slots[slot] = (RESERVATION){pos, bytes, true}; reservations++;
    *actual = bytes; *address = pos; return true;
}
static void RA_Free(RA_ARENA *arena, uintptr_t address, int free_pages)
{
    unsigned i;
    for (i = 0; i < 8; i++) if (arena->slots[i].live && arena->slots[i].start == address) break;
    assert(i < 8 && reservations);
    if (free_pages) MMU_FreePageTables(arena->heap, address, address + arena->slots[i].size, IMG_NULL);
    arena->slots[i].live = false; reservations--;
}
static IMG_BOOL _DeferredAllocPagetables(MMU_HEAP *heap, IMG_DEV_VIRTADDR address, unsigned bytes)
{
    unsigned i, first = address.uiAddr >> 22, last = (address.uiAddr + bytes - 1) >> 22;
    for (i = first; i <= last; i++) {
        MMU_PT_INFO **slot = &heap->psMMUContext->apsPTInfoList[i];
        if (*slot) continue;
        *slot = calloc(1, sizeof(**slot)); assert(*slot); pt_metadata++;
        if (++pt_attempts == fail_pt_at) return false; /* metadata without page backing */
        (*slot)->PTPageCpuVAddr = calloc(1024, 4); assert((*slot)->PTPageCpuVAddr);
        (*slot)->hPTPageOSMemHandle = *slot; pt_pages++;
        ((unsigned *)heap->psMMUContext->pvPDCpuVAddr)[i] = 0x1001 + i * 4096;
    }
    return true;
}

int main(void)
{
    unsigned directory[1024] = {0}, i, iteration;
    PVRSRV_SGXDEV_INFO device = {0};
    MMU_CONTEXT context = {.psDevInfo = &device, .pvPDCpuVAddr = directory};
    DEVICE_MEMORY_HEAP_INFO memory = {0};
    DEV_ARENA_DESCRIPTOR arena_desc = {.BaseDevVAddr = {0xf4000000}, .ui32Size = 0x04fff000,
        .DevMemHeapType = DEVICE_MEMORY_HEAP_SHARED_EXPORTED, .psDeviceMemoryHeapInfo = &memory};
    RA_ARENA arena = {0};
    MMU_HEAP heap = {.psMMUContext = &context, .psDevArena = &arena_desc, .psVMArena = &arena,
        .ui32DataPageSize = 4096, .ui32DataPageMask = 4095, .ui32PDShift = 22,
        .ui32PTShift = 12, .ui32PTMask = 0x003ff000, .ui32PDBaseIndex = 0xf4000000U >> 22,
        .ui32PageTableCount = 20, .ui32PTNumEntriesUsable = 1024, .ui32PTETotalUsable = 0x04fff000 >> 12};
    const __u32 physical[] = {0x2f003000, 0x18008000, 0x01004000, 0x00100000};
    struct sgx545_mmu_check out = {0};
    unsigned capacity = heap.ui32PTETotalUsable;
    device.pvMMUContextList = &context; arena.heap = &heap;

    assert(gma3600_sgx_mmu_exercise(&heap, physical, 3, 0, &out) == -EINVAL);
    assert(gma3600_sgx_mmu_exercise(&heap, physical, 4, 2, &out) == -EINVAL);
    for (iteration = 0; iteration < 3; iteration++) {
        for (i = 0; i < 2; i++) {
            assert(!gma3600_sgx_mmu_exercise(&heap, physical, 4, i, &out));
            assert(out.checked_pages == 4 && out.scatter_breaks == 3 && !out.pt_after);
            assert(out.pt_peak == (i ? 2U : 1U));
            assert(heap.ui32PTETotalUsable == capacity && !pt_pages && !pt_metadata && !reservations);
        }
    }
    fail_reserve = 1;
    assert(gma3600_sgx_mmu_exercise(&heap, physical, 4, 0, &out) == -ENOMEM);
    for (i = 1; i <= 2; i++) {
        pt_attempts = 0; fail_pt_at = i;
        assert(gma3600_sgx_mmu_exercise(&heap, physical, 4, 1, &out) == -ENOMEM);
        assert(!pt_pages && !pt_metadata && !reservations && !out.pt_after);
        assert(heap.ui32PTETotalUsable == capacity);
    }
    fail_pt_at = 0;
    assert(!gma3600_sgx_mmu_exercise(&heap, physical, 4, 1, &out));
    /* Repeated frees in a partial, single-PT heap must preserve capacity too. */
    arena_desc.ui32Size = 127 * 4096;
    heap.ui32PageTableCount = 1; heap.ui32PTETotalUsable = 127;
    for (i = 0; i < 4; i++) {
        assert(!gma3600_sgx_mmu_exercise(&heap, physical, 4, 0, &out));
        assert(heap.ui32PTETotalUsable == 127 && !pt_pages && !pt_metadata && !reservations);
    }
    assert(gma3600_sgx_mmu_exercise(&heap, physical, 4, 1, &out) == -EINVAL);
    puts("Actual DDK MMU tests passed: scatter PTEs, boundary crossing, repeated free capacity and partial PT allocation rollback");
    return 0;
}
