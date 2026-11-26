/* SPDX-License-Identifier: GPL-2.0-only */
/* Included by buffer_manager.c. Use its actual WrapMemory/FreeBuf/BM_Free and
 * getter implementations. The public BM_Wrap policy inherits user map flags
 * and drops coherency/EDM flags; this entry is strictly kernel-only, scatter,
 * externally pinned WC memory, with no user mapping or imported RA ownership. */
#include <linux/errno.h>
#include <linux/string.h>
#include "gma3600_sgx_bm.h"
#include "gma3600_sgx_mmu.h"

int gma3600_sgx_bm_cycle(void *native_heap, const __u32 *physical, __u32 count,
	void *linear, __u32 bytes, __u32 flags, struct sgx545_bm_check *out)
{
	BM_HEAP *heap = native_heap;
	IMG_SYS_PHYADDR *scatter;
	IMG_UINT32 i, iteration, capacity, tables, first = 0;
	IMG_UINT32 native_flags = PVRSRV_HAP_KERNEL_ONLY | PVRSRV_HAP_WRITECOMBINE |
		(flags & SGX545_DEVICE_MMU_FLAGS);
	int ret;

	if (!heap || !physical || !linear || ((IMG_UINTPTR_T)linear & 4095) || !bytes ||
	    bytes > SGX545_BUFFER_MAX_BYTES || !count || count > 256 || count != (bytes + 4095) / 4096 ||
	    !sgx545_device_effective_flags_valid(flags) || !out) return -EINVAL;
	ret = gma3600_sgx_mmu_checkpoint(heap->pMMUHeap, &capacity, &tables);
	if (ret) return ret;
	out->pt_before = tables;
	for (i = 0; i < count; i++) if (!physical[i] || (physical[i] & 4095)) return -EINVAL;
	if (OSAllocMem(PVRSRV_OS_PAGEABLE_HEAP, count * sizeof(*scatter), (void **)&scatter, 0,
		"SGX BM borrowed scatter") != PVRSRV_OK) return -ENOMEM;
	for (i = 0; i < count; i++) scatter[i].uiAddr = physical[i];
	ret = 0;
	for (iteration = 0; iteration < 2; iteration++) {
		BM_BUF *buffer;
		PVRSRV_KERNEL_MEM_INFO memory;
		struct gma_sgx_mmu_binding alias = {0};
		IMG_UINT32 address = 0;
		int cleanup;

		if (OSAllocMem(PVRSRV_OS_PAGEABLE_HEAP, sizeof(*buffer), (void **)&buffer, 0,
			"SGX BM kernel-only alias") != PVRSRV_OK) { ret = -ENOMEM; break; }
		OSMemSet(buffer, 0, sizeof(*buffer));
		if (!WrapMemory(heap, count * 4096, 0, IMG_FALSE, scatter, linear, native_flags, buffer)) {
			OSFreeMem(PVRSRV_OS_PAGEABLE_HEAP, sizeof(*buffer), buffer, IMG_NULL);
			ret = -ENOMEM;
		} else {
			buffer->ui32RefCount = 1;
			address = BM_HandleToDevVaddr(buffer).uiAddr;
			if (!iteration) first = address;
			if (!address || address != first || BM_HandleToCpuVaddr(buffer) != linear ||
			    !BM_HandleToOSMemHandle(buffer)) ret = -EIO;
			OSMemSet(&memory, 0, sizeof(memory)); memory.sMemBlk.hBuffer = buffer;
			for (i = 0; !ret && i < count; i++) {
				IMG_DEV_PHYADDR physical_page;
				BM_GetPhysPageAddr(&memory, (IMG_DEV_VIRTADDR){address + i * 4096}, &physical_page);
				if (physical_page.uiAddr != physical[i]) ret = -EIO;
			}
			alias.heap = heap->pMMUHeap; alias.address = address; alias.bytes = count * 4096;
			alias.pages = count; alias.memory_flags = flags & SGX545_DEVICE_MMU_FLAGS; alias.live = 1;
			if (!ret) ret = gma3600_sgx_mmu_verify(&alias, physical, count, &tables);
			BM_Free(buffer, native_flags); /* Never free the caller's backing RAM. */
		}
		cleanup = gma3600_sgx_mmu_retired(heap->pMMUHeap, address, count * 4096, capacity, &tables);
		out->pt_after = tables;
		if (cleanup || tables != out->pt_before) { ret = -EUCLEAN; break; }
		if (ret) break;
		out->passes++;
	}
	OSFreeMem(PVRSRV_OS_PAGEABLE_HEAP, count * sizeof(*scatter), scatter, IMG_NULL);
	out->alias_address = first;
	return ret;
}
