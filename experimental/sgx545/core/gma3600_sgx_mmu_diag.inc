/* SPDX-License-Identifier: GPL-2.0-only */
/* Included by patched mmu.c, where the native MMU structures are defined. */
#include <linux/errno.h>
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
#endif

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
