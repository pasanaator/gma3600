/* SPDX-License-Identifier: GPL-2.0-only */
/* Included after the native InitDevInfo definition. This gate is intentionally
 * separate from DevInitSGXPart2KM, which remains blocked in prepare-only mode.
 * Each entry requires the bridge and global DDK locks, with no GPU/timer/IRQ
 * path active. PVRSRV_SGXDEV_INFO for these pinned profiles contains no embedded locks.
 */
#include <linux/errno.h>
#include <linux/string.h>
#include "gma3600_sgx_init.h"

/* A single CPU binding owns the native CCB allocation and both device images.
 * While retained, the bridge permits only queries, unbind, abort and disconnect.
 * Existing-heap MMU cleanup may accumulate cache-invalidation bits; preserve
 * those bits at unbind. No power/IRQ/timer or new MMU context can run here. */
struct gma_sgx_native_runtime_state {
	PVRSRV_SGXDEV_INFO *dev, saved, expected;
	PVRSRV_SGX_CCB_INFO *ccb;
	struct sgx545_native_cycle result;
	IMG_UINT32 kicker;
	IMG_BOOL hardware_owned;
};

int gma3600_sgx_native_begin(void *device_node, struct gma_sgx_native_init *snapshot,
	void **runtime, struct sgx545_native_cycle *out)
{
	PVRSRV_DEVICE_NODE *node = device_node;
	PVRSRV_SGXDEV_INFO *dev, *saved;
	struct gma_sgx_native_runtime_state *binding = IMG_NULL;
	PVRSRV_SGX_CCB_INFO *ccb;
	struct sgx545_init_requirements caps = {0};
	SGXMKIF_COMMAND original;
	volatile IMG_UINT32 *words;
	IMG_HANDLE roles[SGX545_INIT_ROLES];
	IMG_UINT32 read, write, kicker, i, flags;
	PVRSRV_ERROR error;
	int ret = 0;

	if (!runtime || *runtime || !out) return -EINVAL;
	if (!node || !node->pvDevice || node->sDevId.eDeviceType != PVRSRV_DEVICE_TYPE_SGX || !snapshot)
		return -EINVAL;
	dev = node->pvDevice;
	if (dev->psKernelCCBInfo || dev->psKernelCCBMemInfo || dev->psKernelCCB ||
	    dev->psKernelCCBCtlMemInfo || dev->psKernelCCBCtl || dev->psKernelCCBEventKickerMemInfo ||
	    dev->pui32KernelCCBEventKicker || dev->psKernelSGXHostCtlMemInfo || dev->psSGXHostCtl ||
	    dev->psKernelSGXTA3DCtlMemInfo || dev->psKernelSGXMiscMemInfo || dev->psKernelHWPerfCBMemInfo ||
	    dev->psKernelTASigBufferMemInfo || dev->psKernel3DSigBufferMemInfo) return -EBUSY;
#if defined(SUPPORT_HW_RECOVERY)
	if (dev->hTimer) return -EBUSY;
#endif
	roles[0] = snapshot->info.hKernelCCBMemInfo; roles[1] = snapshot->info.hKernelCCBCtlMemInfo;
	roles[2] = snapshot->info.hKernelCCBEventKickerMemInfo; roles[3] = snapshot->info.hKernelSGXHostCtlMemInfo;
	roles[4] = snapshot->info.hKernelSGXTA3DCtlMemInfo; roles[5] = snapshot->info.hKernelSGXMiscMemInfo;
	roles[6] = snapshot->info.hKernelHWPerfCBMemInfo; roles[7] = snapshot->info.hKernelTASigBufferMemInfo;
	roles[8] = snapshot->info.hKernel3DSigBufferMemInfo;
	gma_sgx_init_requirements(&caps);
	for (i = 0; i < SGX545_INIT_ROLES; i++) {
		if (roles[i] != &snapshot->memory[i] || !snapshot->memory[i].pvLinAddrKM ||
		    snapshot->memory[i].uAllocSize < caps.minimum_bytes[i] ||
		    !sgx545_device_effective_flags_valid(snapshot->memory[i].ui32Flags)) return -EINVAL;
		if ((snapshot->memory[i].ui32Flags & SGX545_DEVICE_ACCESS) != SGX545_DEVICE_ACCESS) return -EOPNOTSUPP;
	}
	OSMemoryBarrier();
	read = ((PVRSRV_SGX_CCB_CTL *)snapshot->memory[1].pvLinAddrKM)->ui32ReadOffset;
	write = ((PVRSRV_SGX_CCB_CTL *)snapshot->memory[1].pvLinAddrKM)->ui32WriteOffset;
	kicker = *(volatile IMG_UINT32 *)snapshot->memory[2].pvLinAddrKM;
	if (read >= 256 || write >= 256) return -EINVAL;
	if (read != write) return -EAGAIN;
	error = OSAllocMem(PVRSRV_OS_NON_PAGEABLE_HEAP, sizeof(*binding), (IMG_VOID **)&binding, 0,
		"SGX CPU binding state");
	if (error != PVRSRV_OK) return -ENOMEM;
	binding->hardware_owned = IMG_FALSE;
	saved = &binding->saved;
	OSMemCopy(saved, dev, sizeof(*saved));
	error = InitDevInfo(IMG_NULL, node, &snapshot->info);
	if (error != PVRSRV_OK) { ret = -ENOMEM; goto restore; }
	ccb = dev->psKernelCCBInfo;
	if (!ccb || ccb->psCCBMemInfo != &snapshot->memory[0] || ccb->psCCBCtlMemInfo != &snapshot->memory[1] ||
	    ccb->psCommands != ((PVRSRV_SGX_KERNEL_CCB *)snapshot->memory[0].pvLinAddrKM)->asCommands ||
	    ccb->pui32ReadOffset != &((PVRSRV_SGX_CCB_CTL *)snapshot->memory[1].pvLinAddrKM)->ui32ReadOffset ||
	    ccb->pui32WriteOffset != &((PVRSRV_SGX_CCB_CTL *)snapshot->memory[1].pvLinAddrKM)->ui32WriteOffset ||
	    dev->pui32KernelCCBEventKicker != snapshot->memory[2].pvLinAddrKM ||
	    dev->psSGXHostCtl != snapshot->memory[3].pvLinAddrKM ||
	    dev->psKernelSGXTA3DCtlMemInfo != &snapshot->memory[4] || dev->psKernelSGXMiscMemInfo != &snapshot->memory[5] ||
	    dev->psKernelHWPerfCBMemInfo != &snapshot->memory[6] || dev->psKernelTASigBufferMemInfo != &snapshot->memory[7] ||
	    dev->psKernel3DSigBufferMemInfo != &snapshot->memory[8]) { ret = -EIO; goto unbind; }
	out->bound = 1;
	/* Exercise the native CCB pointer, without advancing write/read offsets or
	 * changing the event kicker. No command is published and no kick occurs. */
	OSMemCopy(&original, &ccb->psCommands[write], sizeof(original));
	words = (volatile IMG_UINT32 *)&ccb->psCommands[write];
	for (i = 0; i < sizeof(original) / sizeof(*words); i++) words[i] = 0x43505500U ^ i;
	OSMemoryBarrier();
	for (i = 0; i < sizeof(original) / sizeof(*words); i++)
		if (words[i] != (0x43505500U ^ i)) ret = -EIO;
	OSMemCopy(&ccb->psCommands[write], &original, sizeof(original));
	OSMemoryBarrier();
	if (memcmp(&ccb->psCommands[write], &original, sizeof(original)) ||
	    *ccb->pui32ReadOffset != read || *ccb->pui32WriteOffset != write || *dev->pui32KernelCCBEventKicker != kicker)
		ret = -EUCLEAN;
	if (!ret) out->flags = SGX545_NATIVE_CCB_ROUNDTRIP;
	out->ccb_bytes = sizeof(PVRSRV_SGX_KERNEL_CCB); out->slots = 256;
	out->read_offset = read; out->write_offset = write;
	if (!ret) {
		out->restored = 0;
		binding->dev = dev; binding->ccb = ccb; binding->kicker = kicker;
		dev->ui32CacheControl |= saved->ui32CacheControl;
		OSMemCopy(&binding->expected, dev, sizeof(*dev));
		binding->result = *out; *runtime = binding;
		return 0;
	}
unbind:
	DeinitDevInfo(dev); /* Frees only the newly allocated native CCB metadata. */
restore:
	/* No native references, timers or callbacks may escape this transaction.
	 * Restore all pointer/scalar state, including prior MMU cache-control bits. */
	OSMemCopy(dev, saved, sizeof(*dev));
	flags = memcmp(dev, saved, sizeof(*dev));
	OSFreeMem(PVRSRV_OS_NON_PAGEABLE_HEAP, sizeof(*binding), binding, IMG_NULL);
	if (flags) return -EIO;
	out->restored = 1;
	return ret;
}

int gma3600_sgx_native_query(void *runtime, struct sgx545_native_cycle *out)
{
	struct gma_sgx_native_runtime_state *binding = runtime;
	PVRSRV_SGXDEV_INFO *dev;
	IMG_UINT32 offset = offsetof(PVRSRV_SGXDEV_INFO, ui32CacheControl);

	if (!binding || !out) return -ENOENT;
	if (binding->hardware_owned) return -EBUSY;
	dev = binding->dev;
	/* Only cache-control accumulation by existing-heap PTE cleanup is allowed
	 * between calls. Compare pointers before dereferencing the live CCB. */
	if (memcmp(dev, &binding->expected, offset) ||
	    memcmp((char *)dev + offset + sizeof(dev->ui32CacheControl),
		(char *)&binding->expected + offset + sizeof(dev->ui32CacheControl),
		sizeof(*dev) - offset - sizeof(dev->ui32CacheControl))) return -EUCLEAN;
	if (binding->ccb->psCCBMemInfo != dev->psKernelCCBMemInfo ||
	    binding->ccb->psCCBCtlMemInfo != dev->psKernelCCBCtlMemInfo ||
	    binding->ccb->psCommands != dev->psKernelCCB->asCommands ||
	    binding->ccb->pui32ReadOffset != &dev->psKernelCCBCtl->ui32ReadOffset ||
	    binding->ccb->pui32WriteOffset != &dev->psKernelCCBCtl->ui32WriteOffset) return -EUCLEAN;
	OSMemoryBarrier();
	if (*binding->ccb->pui32ReadOffset != binding->result.read_offset ||
	    *binding->ccb->pui32WriteOffset != binding->result.write_offset ||
	    *dev->pui32KernelCCBEventKicker != binding->kicker) return -EUCLEAN;
	/* Preserve this caller's opaque identifiers, never copy a prior file's. */
	out->flags = binding->result.flags; out->bound = 1; out->restored = 0;
	out->ccb_bytes = binding->result.ccb_bytes; out->slots = binding->result.slots;
	out->read_offset = binding->result.read_offset; out->write_offset = binding->result.write_offset;
	return 0;
}

static int native_end(void **runtime, int accumulate_cache)
{
	struct gma_sgx_native_runtime_state *binding;
	struct sgx545_native_cycle result = {0};
	PVRSRV_SGXDEV_INFO *dev;
	IMG_UINT32 cache;
	int ret;

	if (!runtime || !*runtime) return -ENOENT;
	binding = *runtime; dev = binding->dev;
	if (binding->hardware_owned) return -EBUSY;
	ret = gma3600_sgx_native_query(binding, &result);
	cache = dev->ui32CacheControl;
	/* Free only the CCB allocation owned by this lease, even on a failed
	 * integrity check. No retained reference may outlive its staged RAM. */
	dev->psKernelCCBInfo = binding->ccb;
	DeinitDevInfo(dev);
	OSMemCopy(dev, &binding->saved, sizeof(*dev));
	if (accumulate_cache) dev->ui32CacheControl |= cache;
	OSFreeMem(PVRSRV_OS_NON_PAGEABLE_HEAP, sizeof(*binding), binding, IMG_NULL);
	*runtime = IMG_NULL;
	return ret;
}

int gma3600_sgx_native_end(void **runtime) { return native_end(runtime, 1); }

int gma3600_sgx_native_cycle(void *device_node, struct gma_sgx_native_init *snapshot,
	struct sgx545_native_cycle *out)
{
	void *runtime = IMG_NULL;
	int ret = gma3600_sgx_native_begin(device_node, snapshot, &runtime, out);
	if (ret) return ret;
	ret = native_end(&runtime, 0); /* No interleaved operation in this ioctl. */
	out->restored = 1;
	return ret;
}
