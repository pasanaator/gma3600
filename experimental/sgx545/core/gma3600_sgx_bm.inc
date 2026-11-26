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
		if (!WrapMemory(heap, count * 4096, 0, IMG_FALSE, scatter, linear, native_flags, 4096, buffer)) {
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

/* The modern bridge owns this object and the backing-page pin. The BM object
 * owns VA/PTE and kernel-only OS registration; it never owns the physical RAM.
 * Native legacy meminfo/free/ResMan semantics are deliberately not fabricated. */
struct gma_sgx_bm_owner {
	BM_HEAP *heap;
	BM_BUF *buffer;
	IMG_SYS_PHYADDR *scatter;
	struct gma_sgx_mmu_binding view;
	void *linear;
	IMG_UINT32 bytes, flags, native_flags, capacity;
	int failed;
};

int gma3600_sgx_bm_verify(void *owner, const __u32 *physical, __u32 count, __u32 *tables)
{
	struct gma_sgx_bm_owner *o = owner;
	IMG_UINT32 i;
	PVRSRV_KERNEL_MEM_INFO memory;
	if (!o || o->failed || !o->buffer || count != o->view.pages || !physical) return -EIO;
	if (BM_HandleToCpuVaddr(o->buffer) != o->linear ||
	    BM_HandleToDevVaddr(o->buffer).uiAddr != o->view.address || !BM_HandleToOSMemHandle(o->buffer)) return -EIO;
	OSMemSet(&memory, 0, sizeof(memory)); memory.sMemBlk.hBuffer = o->buffer;
	for (i = 0; i < count; i++) {
		IMG_DEV_PHYADDR page;
		if (o->scatter[i].uiAddr != physical[i]) return -EIO;
		BM_GetPhysPageAddr(&memory, (IMG_DEV_VIRTADDR){o->view.address + i * 4096}, &page);
		if (page.uiAddr != physical[i]) return -EIO;
	}
	return gma3600_sgx_mmu_verify(&o->view, physical, count, tables);
}

int gma3600_sgx_bm_info(void *owner, void *native_memory, int attach)
{
	struct gma_sgx_bm_owner *o = owner;
	PVRSRV_KERNEL_MEM_INFO *m = native_memory;
	IMG_HANDLE os_handle;
	if (!o || o->failed || !o->buffer || !m || m->pvLinAddrKM != o->linear ||
	    m->sDevVAddr.uiAddr != o->view.address || m->uAllocSize != o->bytes || m->ui32Flags != o->flags) return -EIO;
	os_handle = BM_HandleToOSMemHandle(o->buffer);
	if (!os_handle || BM_HandleToCpuVaddr(o->buffer) != o->linear ||
	    BM_HandleToDevVaddr(o->buffer).uiAddr != o->view.address) return -EIO;
	if (attach) {
		m->sMemBlk.hBuffer = o->buffer; m->sMemBlk.hOSMemHandle = os_handle;
		m->sMemBlk.sDevVirtAddr = m->sDevVAddr;
	}
	return m->sMemBlk.hBuffer == o->buffer && m->sMemBlk.hOSMemHandle == os_handle &&
		m->sMemBlk.sDevVirtAddr.uiAddr == o->view.address ? 0 : -EIO;
}

int gma3600_sgx_bm_release(void **owner)
{
	struct gma_sgx_bm_owner *o;
	IMG_UINT32 tables;
	int ret;
	if (!owner || !*owner) return -ENOENT;
	o = *owner;
	if (o->failed) return -EIO;
	BM_Free(o->buffer, o->native_flags); o->buffer = IMG_NULL;
	ret = gma3600_sgx_mmu_retired(o->view.heap, o->view.address, o->view.bytes, o->capacity, &tables);
	if (ret) { o->failed = 1; return -EUCLEAN; }
	OSFreeMem(PVRSRV_OS_PAGEABLE_HEAP, o->view.pages * sizeof(*o->scatter), o->scatter, IMG_NULL);
	OSFreeMem(PVRSRV_OS_PAGEABLE_HEAP, sizeof(*o), o, IMG_NULL);
	*owner = IMG_NULL;
	return 0;
}

int gma3600_sgx_bm_allocate(void *native_heap, const __u32 *physical, __u32 count,
	void *linear, __u32 bytes, __u32 flags, __u32 alignment,
	void **owner, struct gma_sgx_mmu_binding *binding)
{
	struct gma_sgx_bm_owner *o;
	BM_HEAP *heap = native_heap;
	IMG_UINT32 i, capacity, tables;
	int ret, cleanup;
	if (!owner || *owner || !binding || !heap || !linear || ((IMG_UINTPTR_T)linear & 4095) || !physical ||
	    !bytes || bytes > SGX545_BUFFER_MAX_BYTES || !count || count > 256 || count != (bytes + 4095) / 4096 ||
	    alignment < 4096 || alignment > SGX545_BUFFER_MAX_BYTES || (alignment & (alignment - 1)) ||
	    !sgx545_device_effective_flags_valid(flags)) return -EINVAL;
	OSMemSet(binding, 0, sizeof(*binding));
	ret = gma3600_sgx_mmu_checkpoint(heap->pMMUHeap, &capacity, &tables);
	if (ret) return ret;
	for (i = 0; i < count; i++) if (!physical[i] || (physical[i] & 4095)) return -EINVAL;
	if (OSAllocMem(PVRSRV_OS_PAGEABLE_HEAP, sizeof(*o), (void **)&o, 0, "SGX BM owner") != PVRSRV_OK) return -ENOMEM;
	OSMemSet(o, 0, sizeof(*o)); o->heap = heap; o->linear = linear; o->bytes = bytes; o->flags = flags; o->capacity = capacity;
	o->native_flags = PVRSRV_HAP_KERNEL_ONLY | PVRSRV_HAP_WRITECOMBINE | (flags & SGX545_DEVICE_MMU_FLAGS);
	if (OSAllocMem(PVRSRV_OS_PAGEABLE_HEAP, count * sizeof(*o->scatter), (void **)&o->scatter, 0, "SGX BM owned scatter") != PVRSRV_OK) { ret = -ENOMEM; goto free_owner; }
	for (i = 0; i < count; i++) o->scatter[i].uiAddr = physical[i];
	if (OSAllocMem(PVRSRV_OS_PAGEABLE_HEAP, sizeof(*o->buffer), (void **)&o->buffer, 0, "SGX BM retained buffer") != PVRSRV_OK) { ret = -ENOMEM; goto free_scatter; }
	OSMemSet(o->buffer, 0, sizeof(*o->buffer));
	if (!WrapMemory(heap, count * 4096, 0, IMG_FALSE, o->scatter, linear, o->native_flags, alignment, o->buffer)) {
		ret = -ENOMEM;
		OSFreeMem(PVRSRV_OS_PAGEABLE_HEAP, sizeof(*o->buffer), o->buffer, IMG_NULL);
		cleanup = gma3600_sgx_mmu_retired(heap->pMMUHeap, 0, count * 4096, capacity, &tables);
		if (cleanup) { o->buffer = IMG_NULL; o->failed = 1; goto quarantine; }
		goto free_scatter;
	}
	o->buffer->ui32RefCount = 1;
	o->view.heap = heap->pMMUHeap; o->view.address = BM_HandleToDevVaddr(o->buffer).uiAddr;
	o->view.bytes = count * 4096; o->view.pages = count; o->view.live = 1;
	o->view.memory_flags = flags & SGX545_DEVICE_MMU_FLAGS;
	*owner = o; *binding = o->view;
	ret = (o->view.address & (alignment - 1)) ? -EIO : gma3600_sgx_bm_verify(o, physical, count, &tables);
	if (!ret) return 0;
	cleanup = gma3600_sgx_bm_release(owner);
	if (cleanup) { binding->failed = 1; return -EUCLEAN; }
	OSMemSet(binding, 0, sizeof(*binding));
	return ret;
quarantine:
	o->view.heap = heap->pMMUHeap; o->view.bytes = count * 4096; o->view.pages = count; o->view.failed = 1;
	*owner = o; *binding = o->view;
	return -EUCLEAN;
free_scatter:
	OSFreeMem(PVRSRV_OS_PAGEABLE_HEAP, count * sizeof(*o->scatter), o->scatter, IMG_NULL);
free_owner:
	OSFreeMem(PVRSRV_OS_PAGEABLE_HEAP, sizeof(*o), o, IMG_NULL);
	return ret;
}
