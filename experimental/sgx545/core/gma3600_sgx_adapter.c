// SPDX-License-Identifier: GPL-2.0-only
/* CPU-side services and an opt-in diagnostic bridge. No GPU execution. */
#include <linux/atomic.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <drm/drm_device.h>
#include "services_headers.h"
#include "linkage.h"
#include "mutex.h"
#include "lock.h"
#include "mm.h"
#include "mutils.h"
#include "pvr_bridge.h"
#include "sgx545_abi.h"
#include "sgxdefs.h"
#include "sgxinfo.h"
#include "sgxinfokm.h"
#include "gma3600_sgx.h"
#include "gma3600_sgx_memory.h"
#include "gma3600_sgx_prepare.h"
#include "gma3600_sgx_bridge.h"

static_assert(sizeof(PVRSRV_BRIDGE_PACKAGE) == 48);
static_assert(offsetof(PVRSRV_BRIDGE_PACKAGE, hParamOut) == 24);
static_assert(sizeof(struct sgx545_bridge32) == 28);
static_assert(EUR_CR_CORE_ID == GMA3600_SGX_CORE_ID);
static_assert(EUR_CR_CORE_REVISION == GMA3600_SGX_CORE_REV);

struct pci_dev *gpsPVRLDMDev;
struct drm_device *gpsPVRDRMDev;
static bool prepare;
static bool bridge;
static bool native_bm;
module_param(prepare, bool, 0444);
module_param(bridge, bool, 0444);
module_param(native_bm, bool, 0444);
bool gma3600_sgx_native_bm_enabled(void) { return native_bm; }
MODULE_PARM_DESC(native_bm, "Use retained native BM objects for device allocations (requires bridge=1)");
MODULE_PARM_DESC(prepare, "Prepare CPU-side SGX services only; GPU execution stays disabled");
MODULE_PARM_DESC(bridge, "Expose the restricted i386 bridge diagnostic device (requires prepare=1)");

struct gma_sgx_prepared {
	struct device *dev;
	struct gma3600_sgx_device *sgx;
	bool os_ready, mm_ready, sys_ready, ready;
	unsigned int devices;
	struct gma_sgx_bridge *bridge;
};

static DEFINE_MUTEX(prepare_lock);
static bool attempted;
static struct gma_sgx_prepared *active;
static atomic_t blocked_io = ATOMIC_INIT(0);

struct device *PVRLDMGetDevice(void)
{
	return gpsPVRLDMDev ? &gpsPVRLDMDev->dev : NULL;
}

void gma3600_sgx_blocked_write(IMG_UINT32 offset, IMG_UINT32 value)
{
	atomic_inc(&blocked_io);
	pr_err_ratelimited("gma3600 SGX prepare: blocked hardware access offset=%x value=%x\n",
			   offset, value);
}

PVRSRV_ERROR gma3600_sgx_borrow_map(SGX_DEVICE_MAP *map)
{
	struct gma3600_sgx_device *sgx = active->sgx;

	/* Preserve timing fields set by SysInitialise; borrow, never iounmap. */
	map->pvRegsCpuVBase = (void __force *)sgx->regs;
	map->sRegsCpuPBase.uiAddr = sgx->regs_phys;
	map->sRegsSysPBase.uiAddr = sgx->regs_phys;
	map->ui32RegsSize = GMA3600_SGX_REG_SIZE;
	map->ui32IRQ = sgx->pdev->irq;
	map->ui32Flags = 0;
	return PVRSRV_OK;
}

static void gma_sgx_prepare_cleanup(void *data)
{
	struct gma_sgx_prepared *ctx = data;
	PVRSRV_ERROR err = PVRSRV_OK;

	ctx->ready = false;
	if (ctx->sys_ready) {
		err = SysDeinitialise(SysAcquireDataNoCheck());
		ctx->sys_ready = false;
	}
	/* Do not destroy caches underneath a failed device teardown. */
	if (err == PVRSRV_OK) {
		if (ctx->mm_ready)
			LinuxMMCleanup();
		if (ctx->os_ready)
			PVROSFuncDeInit();
	}
	dev_info(ctx->dev, "services cleanup: result=%u pages_live=%lu blocked_hw_accesses=%d\n",
		 err, gma3600_sgx_live_pages(), atomic_read(&blocked_io));
	gpsPVRLDMDev = NULL;
	active = NULL;
}

static ssize_t services_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct gma_sgx_prepared *ctx = dev_get_drvdata(dev);
	struct gma_sgx_bridge_stats stats = {0};

	if (ctx->bridge)
		gma3600_sgx_bridge_stats(ctx->bridge, &stats);
	return sysfs_emit(buf,
		"state=%s devices=%u pages_live=%lu blocked_hw_accesses=%d gpu_execution=disabled bridge=%s files=%u connections=%u bridge_errors=%u heap_handles=%u init_running=%u init_ran=%u buffers=%u buffer_pages=%u buffer_maps=%u buffer_errors=%u device_allocations=%u device_pages=%u device_errors=%u stages=%u staged_buffers=%u staged_pages=%u stage_errors=%u map_leases=%u native_bindings=%u native_errors=%u bm_checks=%u bm_errors=%u allocator=%s bm_allocations=%u bm_pages=%u\n",
		ctx->ready ? "prepared" : "failed", ctx->devices,
		gma3600_sgx_live_pages(), atomic_read(&blocked_io),
		ctx->bridge ? "enabled" : "disabled", stats.files, stats.connections, stats.errors,
		stats.heap_handles, stats.init_running, stats.init_ran,
		stats.buffers, stats.buffer_pages, stats.buffer_maps, stats.buffer_errors,
		stats.device_allocations, stats.device_pages, stats.device_errors,
		stats.stages, stats.staged_buffers, stats.staged_pages, stats.stage_errors, stats.map_leases, stats.native_bindings, stats.native_errors, stats.bm_checks, stats.bm_errors, native_bm ? "native_bm" : "mmu", stats.bm_allocations, stats.bm_pages);
}
static DEVICE_ATTR_RO(services);
static struct attribute *prepare_attrs[] = { &dev_attr_services.attr, NULL };
ATTRIBUTE_GROUPS(prepare);

static int gma_sgx_prepare_probe(struct auxiliary_device *adev,
				const struct auxiliary_device_id *id)
{
	struct gma3600_sgx_device *sgx = container_of(adev,
					struct gma3600_sgx_device, adev);
	PVRSRV_DEVICE_IDENTIFIER devices[PVRSRV_MAX_DEVICES];
	struct gma_sgx_prepared *ctx;
	PVRSRV_ERROR err;
	int ret;

	if (sgx->host_abi != GMA3600_SGX_HOST_ABI)
		return -EPROTONOSUPPORT;
	if (sgx->pdev->device != 0x0be1 || upper_32_bits(sgx->regs_phys) ||
	    pci_channel_offline(sgx->pdev) || sgx->pdev->current_state != PCI_D0)
		return -ENODEV;
	if (readl(sgx->regs + GMA3600_SGX_CORE_ID) != 0x01150000 ||
	    readl(sgx->regs + GMA3600_SGX_CORE_REV) != 0x0001000d)
		return -ENODEV;
	ctx = devm_kzalloc(&adev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->dev = &adev->dev;
	ctx->sgx = sgx;
	mutex_lock(&prepare_lock);
	/* DDK globals are not re-entrant. Reload the module for another run. */
	if (attempted) {
		mutex_unlock(&prepare_lock);
		return -EALREADY;
	}
	attempted = true;
	active = ctx;
	gpsPVRLDMDev = sgx->pdev;
	mutex_unlock(&prepare_lock);
	dev_set_drvdata(&adev->dev, ctx);
	ret = devm_add_action_or_reset(&adev->dev, gma_sgx_prepare_cleanup, ctx);
	if (ret)
		return ret;
	PVRDPFInit();
	LinuxInitMutex(&gPVRSRVLock);
	err = PVROSFuncInit();
	if (err != PVRSRV_OK)
		goto fail;
	ctx->os_ready = true;
	PVRLinuxMUtilsInit();
	err = LinuxMMInit();
	if (err != PVRSRV_OK)
		goto fail;
	ctx->mm_ready = true;
	/* No procfs, mmap, bridge dispatch, IRQ, firmware or device power setup. */
	err = SysInitialise();
	if (err != PVRSRV_OK)
		goto fail;
	ctx->sys_ready = true;
	err = PVRSRVEnumerateDevicesKM(&ctx->devices, devices);
	if (err != PVRSRV_OK || ctx->devices != 1 ||
	    devices[0].eDeviceType != PVRSRV_DEVICE_TYPE_SGX ||
	    gma3600_sgx_live_pages() < 4 || atomic_read(&blocked_io))
		goto fail;
	if (bridge) {
		ret = gma3600_sgx_bridge_register(&adev->dev, &ctx->bridge);
		if (ret) {
			dev_err(&adev->dev, "bridge registration failed: %d\n", ret);
			goto fail;
		}
	}
	ctx->ready = true;
	dev_info(&adev->dev, "services prepared: devices=%u pages_live=%lu; GPU execution disabled\n",
		 ctx->devices, gma3600_sgx_live_pages());
	return 0;
fail:
	dev_err(&adev->dev, "services preparation failed: vendor_error=%u blocked_hw_accesses=%d\n",
		 err, atomic_read(&blocked_io));
	return -EIO;
}

static const struct auxiliary_device_id prepare_ids[] = {
	{ .name = "gma3600.sgx" }, { }
};
MODULE_DEVICE_TABLE(auxiliary, prepare_ids);
static struct auxiliary_driver prepare_driver = {
	.name = "gma3600_sgx_services",
	.probe = gma_sgx_prepare_probe,
	.id_table = prepare_ids,
	.driver = { .dev_groups = prepare_groups },
};

static int __init gma_sgx_init(void)
{
	if (!prepare)
		return -EOPNOTSUPP;
	return auxiliary_driver_register(&prepare_driver);
}
static void __exit gma_sgx_exit(void)
{
	auxiliary_driver_unregister(&prepare_driver);
}
module_init(gma_sgx_init);
module_exit(gma_sgx_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SGX545 CPU-side services preparation; GPU execution disabled");
MODULE_VERSION("0.17.0");
