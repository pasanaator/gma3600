/* SPDX-License-Identifier: GPL-2.0-only */
/* Included by patched mmu.c, where the native MMU structures are defined. */
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "gma3600_sgx_mmu.h"

/* Only the tested SGX545 4K, non-bypass configuration is supported here. */
#if !defined(FIX_HW_BRN_31620) && !defined(SUPPORT_SGX_MMU_DUMMY_PAGE) && !defined(SUPPORT_SGX_MMU_BYPASS)
static IMG_UINT32 gma_mmu_table_count(MMU_HEAP *heap)
{
	IMG_UINT32 i, count = 0;

	for (i = 0; i < heap->ui32PageTableCount; i++)
		count += heap->psMMUContext->apsPTInfoList[heap->ui32PDBaseIndex + i] != IMG_NULL;
	return count;
}

/* MMU_Alloc can leave empty PT metadata/pages after a partial allocation.
 * Reclaim only empty entries; mapped page tables are never swept here.
 */
static void gma_mmu_trim_empty(MMU_HEAP *heap)
{
	IMG_UINT32 i;

	for (i = 0; i < heap->ui32PageTableCount; i++) {
		MMU_PT_INFO *pt = heap->psMMUContext->apsPTInfoList[heap->ui32PDBaseIndex + i];

		if (pt && !pt->ui32ValidPTECount)
			_DeferredFreePageTable(heap, i, IMG_TRUE);
	}
	MMU_InvalidateDirectoryCache(heap->psMMUContext->psDevInfo);
}

static int gma_mmu_page_matches(MMU_HEAP *heap, IMG_DEV_VIRTADDR address, IMG_UINT32 physical)
{
	MMU_PT_INFO *pt = heap->psMMUContext->apsPTInfoList[address.uiAddr >> heap->ui32PDShift];
	IMG_UINT32 index = (address.uiAddr & heap->ui32PTMask) >> heap->ui32PTShift;

	if (!pt || !pt->PTPageCpuVAddr ||
	    !(((IMG_UINT32 *)pt->PTPageCpuVAddr)[index] & SGX_MMU_PTE_VALID))
		return 0;
	return MMU_GetPhysPageAddr(heap, address).uiAddr == physical;
}

static int gma_mmu_attributes_match(MMU_HEAP *heap, IMG_DEV_VIRTADDR address, IMG_UINT32 flags)
{
	MMU_PT_INFO *pt = heap->psMMUContext->apsPTInfoList[address.uiAddr >> heap->ui32PDShift];
	IMG_UINT32 index = (address.uiAddr & heap->ui32PTMask) >> heap->ui32PTShift;
	IMG_UINT32 expected = 0, mask = SGX_MMU_PTE_READONLY | SGX_MMU_PTE_WRITEONLY |
		SGX_MMU_PTE_CACHECONSISTENT | SGX_MMU_PTE_EDMPROTECT;
	if (!pt || !pt->PTPageCpuVAddr) return 0;
	if ((flags & SGX545_DEVICE_ACCESS) == PVRSRV_MEM_READ) expected |= SGX_MMU_PTE_READONLY;
	if ((flags & SGX545_DEVICE_ACCESS) == PVRSRV_MEM_WRITE) expected |= SGX_MMU_PTE_WRITEONLY;
	if (flags & PVRSRV_MEM_CACHE_CONSISTENT) expected |= SGX_MMU_PTE_CACHECONSISTENT;
	if (flags & PVRSRV_MEM_EDM_PROTECT) expected |= SGX_MMU_PTE_EDMPROTECT;
	return (((IMG_UINT32 *)pt->PTPageCpuVAddr)[index] & mask) == expected;
}
#endif

int gma3600_sgx_mmu_verify(struct gma_sgx_mmu_binding *binding, const __u32 *physical,
	__u32 count, __u32 *tables)
{
#if defined(FIX_HW_BRN_31620) || defined(SUPPORT_SGX_MMU_DUMMY_PAGE) || defined(SUPPORT_SGX_MMU_BYPASS)
	return -EOPNOTSUPP;
#else
	MMU_HEAP *heap = binding->heap;
	IMG_DEV_VIRTADDR walk;
	IMG_UINT32 i;

	if (!binding->live || binding->failed || !physical || count != binding->pages)
		return -EINVAL;
	OSMemoryBarrier();
	for (i = 0; i < count; i++) {
		walk.uiAddr = binding->address + i * 4096;
		if (!gma_mmu_page_matches(heap, walk, physical[i]) ||
		    !gma_mmu_attributes_match(heap, walk, binding->memory_flags))
			return -EIO;
	}
	*tables = gma_mmu_table_count(heap);
	return 0;
#endif
}

int gma3600_sgx_mmu_unbind(struct gma_sgx_mmu_binding *binding)
{
#if defined(FIX_HW_BRN_31620) || defined(SUPPORT_SGX_MMU_DUMMY_PAGE) || defined(SUPPORT_SGX_MMU_BYPASS)
	return -EOPNOTSUPP;
#else
	MMU_HEAP *heap = binding->heap;
	IMG_UINT32 i, capacity;

	if (binding->failed)
		return -EIO; /* Do not retry RA_Free on an already released reservation. */
	if (!binding->live)
		return 0;
	capacity = heap->ui32PTETotalUsable;
	MMU_Free(heap, (IMG_DEV_VIRTADDR){binding->address}, binding->bytes);
	binding->live = 0;
	OSMemoryBarrier();
	for (i = 0; i < binding->pages; i++) {
		IMG_UINT32 address = binding->address + i * 4096;
		MMU_PT_INFO *pt = heap->psMMUContext->apsPTInfoList[address >> heap->ui32PDShift];
		IMG_UINT32 index = (address & heap->ui32PTMask) >> heap->ui32PTShift;

		if (pt && pt->PTPageCpuVAddr && (((IMG_UINT32 *)pt->PTPageCpuVAddr)[index] & SGX_MMU_PTE_VALID))
			binding->failed = 1;
	}
	gma_mmu_trim_empty(heap);
	OSMemoryBarrier();
	if (heap->ui32PTETotalUsable != capacity)
		binding->failed = 1;
	return binding->failed ? -EIO : 0;
#endif
}

int gma3600_sgx_mmu_bind(void *native_heap, const __u32 *physical, __u32 count,
	__u32 alignment, struct gma_sgx_mmu_binding *binding)
{
	return gma3600_sgx_mmu_bind_flags(native_heap, physical, count, alignment,
		PVRSRV_MEM_READ | PVRSRV_MEM_WRITE, binding);
}

int gma3600_sgx_mmu_bind_flags(void *native_heap, const __u32 *physical, __u32 count,
	__u32 alignment, __u32 memory_flags, struct gma_sgx_mmu_binding *binding)
{
#if defined(FIX_HW_BRN_31620) || defined(SUPPORT_SGX_MMU_DUMMY_PAGE) || defined(SUPPORT_SGX_MMU_BYPASS)
	return -EOPNOTSUPP;
#else
	MMU_HEAP *heap = native_heap;
	IMG_SYS_PHYADDR *scatter;
	IMG_DEV_VIRTADDR address;
	IMG_SIZE_T actual = 0;
	IMG_UINT32 i, tables, bytes;
	int ret = -ENOMEM;

	if (binding->live || binding->failed)
		return -EBUSY;
	memset(binding, 0, sizeof(*binding));
	if ((memory_flags & ~SGX545_DEVICE_MMU_FLAGS) ||
	    !(memory_flags & (PVRSRV_MEM_READ | PVRSRV_MEM_WRITE)))
		return -EINVAL;
#if defined(FIX_HW_BRN_25503)
	if (memory_flags & PVRSRV_MEM_EDM_PROTECT) return -EOPNOTSUPP;
#endif
	if (!heap || !physical || !count || count > 256 || alignment < 4096 ||
	    alignment > SGX545_BUFFER_MAX_BYTES || (alignment & (alignment - 1)))
		return -EINVAL;
	if (heap->ui32DataPageSize != 4096 || heap->ui32PDShift != 22 ||
	    heap->psDevArena->psDeviceMemoryHeapInfo->psLocalDevMemArena ||
	    (heap->psDevArena->DevMemHeapType != DEVICE_MEMORY_HEAP_SHARED &&
	     heap->psDevArena->DevMemHeapType != DEVICE_MEMORY_HEAP_SHARED_EXPORTED))
		return -EOPNOTSUPP;
	bytes = count * 4096;
	if (bytes > heap->psDevArena->ui32Size)
		return -ENOSPC;
	for (i = 0; i < count; i++)
		if (physical[i] & 4095)
			return -EINVAL;
	scatter = kcalloc(count, sizeof(*scatter), GFP_KERNEL);
	if (!scatter)
		return -ENOMEM;
	for (i = 0; i < count; i++) scatter[i].uiAddr = physical[i];
	if (!MMU_Alloc(heap, bytes, &actual, 0, alignment, &address)) {
		gma_mmu_trim_empty(heap);
		OSMemoryBarrier();
		goto out;
	}
	binding->heap = heap; binding->address = address.uiAddr;
	binding->bytes = bytes; binding->pages = count; binding->live = 1;
	binding->memory_flags = memory_flags;
	if (actual != bytes || (address.uiAddr & (alignment - 1))) {
		ret = -EIO;
		goto rollback;
	}
	MMU_MapScatter(heap, address, scatter, bytes, memory_flags, IMG_NULL);
	ret = gma3600_sgx_mmu_verify(binding, physical, count, &tables);
	if (!ret)
		goto out;
rollback:
	if (gma3600_sgx_mmu_unbind(binding)) ret = -EIO;
out:
	kfree(scatter);
	return ret;
#endif
}

int gma3600_sgx_mmu_checkpoint(void *native_heap, __u32 *capacity, __u32 *tables)
{
#if defined(FIX_HW_BRN_31620) || defined(SUPPORT_SGX_MMU_DUMMY_PAGE) || defined(SUPPORT_SGX_MMU_BYPASS)
	return -EOPNOTSUPP;
#else
	MMU_HEAP *heap = native_heap;
	if (!heap || heap->ui32DataPageSize != 4096 || heap->ui32PDShift != 22 ||
	    heap->psDevArena->psDeviceMemoryHeapInfo->psLocalDevMemArena ||
	    (heap->psDevArena->DevMemHeapType != DEVICE_MEMORY_HEAP_SHARED &&
	     heap->psDevArena->DevMemHeapType != DEVICE_MEMORY_HEAP_SHARED_EXPORTED)) return -EOPNOTSUPP;
	*capacity = heap->ui32PTETotalUsable; *tables = gma_mmu_table_count(heap);
	return 0;
#endif
}

int gma3600_sgx_mmu_retired(void *native_heap, __u32 address, __u32 bytes, __u32 capacity, __u32 *tables)
{
#if defined(FIX_HW_BRN_31620) || defined(SUPPORT_SGX_MMU_DUMMY_PAGE) || defined(SUPPORT_SGX_MMU_BYPASS)
	return -EOPNOTSUPP;
#else
	MMU_HEAP *heap = native_heap;
	IMG_UINT32 i, ignored;
	if (gma3600_sgx_mmu_checkpoint(heap, &ignored, tables)) return -EOPNOTSUPP;
	if (address && ((address & 4095) || !bytes || (bytes & 4095) ||
	    (IMG_UINT64)address + bytes > 0x100000000ULL)) return -EINVAL;
	OSMemoryBarrier();
	for (i = 0; address && i < bytes; i += 4096) {
		IMG_UINT32 page = address + i;
		MMU_PT_INFO *pt = heap->psMMUContext->apsPTInfoList[page >> heap->ui32PDShift];
		IMG_UINT32 index = (page & heap->ui32PTMask) >> heap->ui32PTShift;
		if (pt && pt->PTPageCpuVAddr && (((IMG_UINT32 *)pt->PTPageCpuVAddr)[index] & SGX_MMU_PTE_VALID)) return -EUCLEAN;
	}
	gma_mmu_trim_empty(heap);
	*tables = gma_mmu_table_count(heap);
	return heap->ui32PTETotalUsable == capacity ? 0 : -EUCLEAN;
#endif
}

int gma3600_sgx_mmu_exercise(void *native_heap, const __u32 *physical, __u32 count,
	__u32 flags, struct sgx545_mmu_check *result)
{
#if defined(FIX_HW_BRN_31620) || defined(SUPPORT_SGX_MMU_DUMMY_PAGE) || defined(SUPPORT_SGX_MMU_BYPASS)
	return -EOPNOTSUPP;
#else
	MMU_HEAP *heap = native_heap;
	IMG_SYS_PHYADDR scatter[4];
	IMG_DEV_VIRTADDR address = {0}, guard = {0}, walk;
	IMG_SIZE_T actual;
	IMG_UINT32 i, bytes, boundary, guard_bytes = 0, capacity;
	int ret = -ENOMEM;
	IMG_BOOL allocated = IMG_FALSE, guarded = IMG_FALSE;

	result->device_address = result->checked_pages = result->pt_peak = 0;
	result->pt_after = result->scatter_breaks = 0;
	if (!heap || !physical || count != 4 || flags & ~SGX545_MMU_CROSS_BOUNDARY)
		return -EINVAL;
	if (heap->ui32DataPageSize != 4096 || heap->ui32PDShift != 22 ||
	    heap->psDevArena->psDeviceMemoryHeapInfo->psLocalDevMemArena ||
	    (heap->psDevArena->DevMemHeapType != DEVICE_MEMORY_HEAP_SHARED &&
	     heap->psDevArena->DevMemHeapType != DEVICE_MEMORY_HEAP_SHARED_EXPORTED))
		return -EOPNOTSUPP;
	for (i = 0; i < count; i++) {
		if (physical[i] & 4095)
			return -EINVAL;
		scatter[i].uiAddr = physical[i];
		if (i && physical[i] != physical[i - 1] + 4096U)
			result->scatter_breaks++;
	}
	if (gma_mmu_table_count(heap))
		return -EBUSY; /* Do not disturb another allocator's page tables. */
	capacity = heap->ui32PTETotalUsable;
	bytes = count * 4096;
	boundary = 1U << heap->ui32PDShift;
	if (flags & SGX545_MMU_CROSS_BOUNDARY) {
		if (heap->psDevArena->ui32Size < 2 * boundary ||
		    heap->psDevArena->BaseDevVAddr.uiAddr & (boundary - 1))
			return -EINVAL;
		guard_bytes = boundary - 8192;
		actual = 0;
		if (!MMU_Alloc(heap, guard_bytes, &actual, 0, boundary, &guard))
			goto cleanup;
		guarded = IMG_TRUE;
		if (actual != guard_bytes || guard.uiAddr != heap->psDevArena->BaseDevVAddr.uiAddr) {
			ret = -EBUSY;
			goto cleanup;
		}
	}
	actual = 0;
	if (!MMU_Alloc(heap, bytes, &actual, 0, 4096, &address))
		goto cleanup;
	allocated = IMG_TRUE;
	if (actual != bytes || (guarded && address.uiAddr != guard.uiAddr + guard_bytes)) {
		ret = -EIO;
		goto cleanup;
	}
	MMU_MapScatter(heap, address, scatter, bytes, PVRSRV_MEM_READ | PVRSRV_MEM_WRITE, IMG_NULL);
	OSMemoryBarrier(); /* Drain WC table writes before CPU readback. */
	result->device_address = address.uiAddr;
	result->pt_peak = gma_mmu_table_count(heap);
	ret = 0;
	for (i = 0; i < count; i++) {
		walk.uiAddr = address.uiAddr + i * 4096;
		if (!gma_mmu_page_matches(heap, walk, physical[i])) {
			ret = -EIO;
			goto cleanup;
		}
		result->checked_pages++;
	}
	if (guarded) {
		MMU_Free(heap, guard, guard_bytes);
		guarded = IMG_FALSE;
		OSMemoryBarrier();
		/* Freeing the adjacent reserved VA must not remove mapped PTEs. */
		for (i = 0; i < count; i++) {
			walk.uiAddr = address.uiAddr + i * 4096;
			if (!gma_mmu_page_matches(heap, walk, physical[i])) {
				ret = -EIO;
				goto cleanup;
			}
		}
	}
cleanup:
	if (allocated)
		MMU_Free(heap, address, bytes);
	if (guarded)
		MMU_Free(heap, guard, guard_bytes);
	gma_mmu_trim_empty(heap);
	OSMemoryBarrier();
	result->pt_after = gma_mmu_table_count(heap);
	if (result->pt_after || heap->ui32PTETotalUsable != capacity)
		return -EIO;
	return ret;
#endif
}

/* A correct PTE walk is insufficient if reset installs a different PD. */
int gma3600_sgx_mmu_root_matches(void *handle, __u32 physical)
{
	MMU_HEAP *heap = handle;
	if (!heap || !heap->psMMUContext || !physical || (physical & 4095)) return -EINVAL;
	return heap->psMMUContext->sPDDevPAddr.uiAddr == physical ? 0 : -EIO;
}
