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
    IMG_DEV_PHYADDR sPDDevPAddr;
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
#define PVRSRV_MEM_CACHE_CONSISTENT SGX545_DEVICE_COHERENT
#define PVRSRV_MEM_EDM_PROTECT SGX545_DEVICE_EDM_PROTECT
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
static unsigned pt_pages, pt_metadata, reservations, fail_pt_at, pt_attempts, fail_reserve, reserve_floor, corrupt_free_capacity;
#define GFP_KERNEL 0
static unsigned fail_scatter;
static void *kcalloc(size_t n, size_t size, int flags)
{ if (fail_scatter) { fail_scatter--; return NULL; } return calloc(n, size); }
#define kfree free
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
    if (reserve_floor) pos += reserve_floor;
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
    if (corrupt_free_capacity) { arena->heap->ui32PTETotalUsable--; corrupt_free_capacity = 0; }
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

static void persistent_bindings(MMU_HEAP *heap, const __u32 *physical)
{
    struct gma_sgx_mmu_binding a = {0}, b = {0}, c = {0};
    __u32 tables, old_address;
    unsigned i, capacity = heap->ui32PTETotalUsable;
    __u32 full[256];

    assert(gma3600_sgx_mmu_bind(heap, physical, 0, 4096, &a) == -EINVAL);
    assert(gma3600_sgx_mmu_bind(heap, physical, 4, 8193, &a) == -EINVAL);
    assert(gma3600_sgx_mmu_bind(heap, physical, 257, 4096, &a) == -EINVAL);
    assert(!gma3600_sgx_mmu_bind_flags(heap, physical, 1, 4096, 3 | PVRSRV_MEM_CACHE_CONSISTENT | PVRSRV_MEM_EDM_PROTECT, &a));
    assert(a.pages == 1 && a.bytes == 4096);
    assert(!gma3600_sgx_mmu_verify(&a, physical, 1, &tables));
    assert(!gma3600_sgx_mmu_unbind(&a) && !pt_pages && !pt_metadata && !reservations);
    assert(gma3600_sgx_mmu_bind_flags(heap, physical, 4, 4096, 0, &a) == -EINVAL);
    assert(gma3600_sgx_mmu_bind_flags(heap, physical, 4, 4096, PVRSRV_MEM_CACHE_CONSISTENT, &a) == -EINVAL);
    assert(gma3600_sgx_mmu_bind_flags(heap, physical, 4, 4096, 0x83, &a) == -EINVAL);
    for (i = 0; i < 12; i++) {
        unsigned access = i / 4 + 1;
        unsigned flags = access | (i & 1 ? PVRSRV_MEM_CACHE_CONSISTENT : 0) |
            (i & 2 ? PVRSRV_MEM_EDM_PROTECT : 0);
        unsigned count = i & 1 ? 2 : 4;
        unsigned expected = (i & 1 ? SGX_MMU_PTE_CACHECONSISTENT : 0) |
            (i & 2 ? SGX_MMU_PTE_EDMPROTECT : 0) |
            (access == PVRSRV_MEM_READ ? SGX_MMU_PTE_READONLY : 0) |
            (access == PVRSRV_MEM_WRITE ? SGX_MMU_PTE_WRITEONLY : 0);
        unsigned mask = SGX_MMU_PTE_CACHECONSISTENT | SGX_MMU_PTE_EDMPROTECT |
            SGX_MMU_PTE_READONLY | SGX_MMU_PTE_WRITEONLY;
        MMU_PT_INFO *pt;
        unsigned *pte;
        assert(!gma3600_sgx_mmu_bind_flags(heap, physical, count, 4096, flags, &a));
        assert(a.memory_flags == flags);
        assert(!gma3600_sgx_mmu_bind(heap, physical, 4, 4096, &b));
        pt = heap->psMMUContext->apsPTInfoList[a.address >> heap->ui32PDShift];
        pte = &((unsigned *)pt->PTPageCpuVAddr)[(a.address & heap->ui32PTMask) >> heap->ui32PTShift];
        assert((*pte & mask) == expected);
        assert(!gma3600_sgx_mmu_verify(&a, physical, count, &tables));
        *pte ^= SGX_MMU_PTE_CACHECONSISTENT;
        assert(gma3600_sgx_mmu_verify(&a, physical, count, &tables) == -EIO);
        *pte ^= SGX_MMU_PTE_CACHECONSISTENT;
        *pte ^= SGX_MMU_PTE_EDMPROTECT;
        assert(gma3600_sgx_mmu_verify(&a, physical, count, &tables) == -EIO);
        *pte ^= SGX_MMU_PTE_EDMPROTECT;
        *pte ^= SGX_MMU_PTE_READONLY;
        assert(gma3600_sgx_mmu_verify(&a, physical, count, &tables) == -EIO);
        *pte ^= SGX_MMU_PTE_READONLY;
        *pte ^= SGX_MMU_PTE_WRITEONLY;
        assert(gma3600_sgx_mmu_verify(&a, physical, count, &tables) == -EIO);
        *pte ^= SGX_MMU_PTE_WRITEONLY;
        assert(!gma3600_sgx_mmu_verify(&a, physical, count, &tables));
        assert(!gma3600_sgx_mmu_unbind(&a));
        assert(!gma3600_sgx_mmu_verify(&b, physical, 4, &tables));
        assert(!gma3600_sgx_mmu_unbind(&b));
        assert(!pt_pages && !pt_metadata && !reservations);
    }
    for (i = 0; i < 4; i++) {
        assert(!gma3600_sgx_mmu_bind(heap, physical, 4, 4096, &a));
        old_address = a.address;
        assert(gma3600_sgx_mmu_bind(heap, physical, 4, 4096, &a) == -EBUSY && a.live);
        assert(!gma3600_sgx_mmu_bind(heap, physical, 4, 4096, &b));
        assert(a.address != b.address && reservations == 2 && pt_pages == 1);
        assert(!gma3600_sgx_mmu_verify(&a, physical, 4, &tables) && tables == 1);
        assert(!gma3600_sgx_mmu_verify(&b, physical, 4, &tables));
        fail_reserve = 1;
        assert(gma3600_sgx_mmu_bind(heap, physical, 4, 4096, &c) == -ENOMEM);
        fail_scatter = 1;
        assert(gma3600_sgx_mmu_bind(heap, physical, 4, 4096, &c) == -ENOMEM);
        /* Fail a new page table while an existing one still has live PTEs. */
        if (heap->psDevArena->ui32Size > 0x400000) {
            reserve_floor = 0x400000; fail_pt_at = pt_attempts + 1;
            assert(gma3600_sgx_mmu_bind(heap, physical, 4, 0x100000, &c) == -ENOMEM);
            reserve_floor = fail_pt_at = 0;
            assert(pt_pages == 1 && pt_metadata == 1 && reservations == 2);
        }
        assert(!gma3600_sgx_mmu_verify(&b, physical, 4, &tables));
        assert(!gma3600_sgx_mmu_unbind(&a) && !a.live && pt_pages == 1);
        assert(!gma3600_sgx_mmu_unbind(&a)); /* no double RA_Free */
        assert(gma3600_sgx_mmu_verify(&a, physical, 4, &tables) == -EINVAL);
        assert(!gma3600_sgx_mmu_verify(&b, physical, 4, &tables));
        assert(!gma3600_sgx_mmu_bind(heap, physical, 4, 4096, &c) && c.address == old_address);
        assert(!gma3600_sgx_mmu_unbind(&b) && pt_pages == 1);
        assert(!gma3600_sgx_mmu_verify(&c, physical, 4, &tables));
        assert(!gma3600_sgx_mmu_unbind(&c));
        assert(!pt_pages && !pt_metadata && !reservations && heap->ui32PTETotalUsable == capacity);
    }
    if (heap->psDevArena->ui32Size >= sizeof(full) * 1024) {
        for (i = 0; i < 256; i++) full[i] = 0x1000000 + i * 8192;
        assert(!gma3600_sgx_mmu_bind(heap, full, 256, 0x100000, &a));
        assert(a.pages == 256 && !(a.address & 0xfffff));
        assert(!gma3600_sgx_mmu_verify(&a, full, 256, &tables));
        full[255] += 4096;
        assert(gma3600_sgx_mmu_verify(&a, full, 256, &tables) == -EIO);
        assert(!gma3600_sgx_mmu_unbind(&a));
    }
    assert(!gma3600_sgx_mmu_bind(heap, physical, 4, 4096, &a));
    corrupt_free_capacity = 1;
    assert(gma3600_sgx_mmu_unbind(&a) == -EIO && a.failed && !a.live);
    assert(!pt_pages && !pt_metadata && !reservations);
    /* A failed post-free check must never invoke RA_Free twice. */
    assert(gma3600_sgx_mmu_unbind(&a) == -EIO);
    assert(gma3600_sgx_mmu_bind(heap, physical, 4, 4096, &a) == -EBUSY);
    heap->ui32PTETotalUsable = capacity; /* test-only restoration of injected corruption */

}

int main(void)
{
    MMU_CONTEXT root_context = {.sPDDevPAddr = {0x12345000}};
    MMU_HEAP root_heap = {.psMMUContext=&root_context};
    assert(!gma3600_sgx_mmu_root_matches(&root_heap,0x12345000));
    assert(gma3600_sgx_mmu_root_matches(&root_heap,0x12346000)==-EIO);
    assert(gma3600_sgx_mmu_root_matches(&root_heap,0x12345001)==-EINVAL);
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
    persistent_bindings(&heap, physical);
    {
        struct gma_sgx_mmu_binding alias = {0};
        unsigned before, tables, address;
        assert(!gma3600_sgx_mmu_checkpoint(&heap, &before, &tables) && !tables);
        assert(!gma3600_sgx_mmu_bind_flags(&heap, physical, 4, 4096, PVRSRV_MEM_READ, &alias));
        address = alias.address;
        assert(gma3600_sgx_mmu_retired(&heap, address, alias.bytes, before, &tables) == -EUCLEAN);
        assert(alias.live && reservations && pt_pages); /* An observation never unmaps live pages. */
        assert(!gma3600_sgx_mmu_unbind(&alias));
        assert(!gma3600_sgx_mmu_retired(&heap, address, 16384, before, &tables) && !tables);
        assert(gma3600_sgx_mmu_retired(&heap, address, 16384, before - 1, &tables) == -EUCLEAN);
        assert(gma3600_sgx_mmu_retired(&heap, address + 1, 16384, before, &tables) == -EINVAL);
    }
    /* Repeated frees in a partial, single-PT heap must preserve capacity too. */
    arena_desc.ui32Size = 127 * 4096;
    heap.ui32PageTableCount = 1; heap.ui32PTETotalUsable = 127;
    for (i = 0; i < 4; i++) {
        assert(!gma3600_sgx_mmu_exercise(&heap, physical, 4, 0, &out));
        assert(heap.ui32PTETotalUsable == 127 && !pt_pages && !pt_metadata && !reservations);
    }
    assert(gma3600_sgx_mmu_exercise(&heap, physical, 4, 1, &out) == -EINVAL);
    persistent_bindings(&heap, physical);
    puts("Actual DDK MMU tests passed: persistent sibling allocations, VA reuse, scatter PTEs, boundary crossing, repeated free capacity and partial PT allocation rollback");
    return 0;
}
