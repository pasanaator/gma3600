// SPDX-License-Identifier: GPL-2.0-only
#include <linux/idr.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "gma3600_drv.h"
#include "gma3600_sgx.h"

static DEFINE_IDA(gma3600_sgx_ids);
static bool sgx_host;
module_param(sgx_host, bool, 0444);
MODULE_PARM_DESC(sgx_host, "Publish experimental SGX identification companion (default off)");

static void gma3600_sgx_release(struct device *dev)
{
	struct auxiliary_device *adev = to_auxiliary_dev(dev);
	struct gma3600_sgx_device *sgx = container_of(adev,
					struct gma3600_sgx_device, adev);

	ida_free(&gma3600_sgx_ids, adev->id);
	kfree(sgx);
}

int gma3600_sgx_host_init(struct gma3600_device *gdev)
{
	struct gma3600_sgx_device *sgx;
	int id, ret;

	if (!sgx_host)
		return 0;
	if (gdev->mmio_size < GMA3600_SGX_REG_OFFSET + GMA3600_SGX_REG_SIZE)
		return -ENODEV;
	sgx = kzalloc(sizeof(*sgx), GFP_KERNEL);
	if (!sgx)
		return -ENOMEM;
	id = ida_alloc(&gma3600_sgx_ids, GFP_KERNEL);
	if (id < 0) {
		kfree(sgx);
		return id;
	}
	sgx->adev.name = "sgx";
	sgx->adev.id = id;
	sgx->adev.dev.parent = &gdev->pdev->dev;
	sgx->adev.dev.release = gma3600_sgx_release;
	sgx->pdev = gdev->pdev;
	sgx->regs = gdev->mmio + GMA3600_SGX_REG_OFFSET;
	sgx->regs_phys = gdev->mmio_phys + GMA3600_SGX_REG_OFFSET;
	sgx->host_abi = GMA3600_SGX_HOST_ABI;
	sgx->host = gdev;
	sgx->irq_ops = &gma3600_sgx_irq_ops;
	sgx->lease_ops = &gma3600_sgx_lease_ops;
	gma3600_sgx_irq_init(gdev);
	ret = auxiliary_device_init(&sgx->adev);
	if (ret) {
		ida_free(&gma3600_sgx_ids, id);
		kfree(sgx);
		return ret;
	}
	ret = auxiliary_device_add(&sgx->adev);
	if (ret) {
		auxiliary_device_uninit(&sgx->adev);
		return ret;
	}
	gdev->sgx = sgx;
	return 0;
}

void gma3600_sgx_host_fini(struct gma3600_device *gdev)
{
	gma3600_sgx_remove_begin(gdev);
	if (!gdev->sgx)
		return;
	/* Let the child drain jobs with IRQs available, unregister its callback,
	 * cancel deferred work and free its data, in that order. Closing the IRQ
	 * gate first could deadlock a valid remove callback waiting for completion.
	 */
	auxiliary_device_delete(&gdev->sgx->adev);
	gma3600_sgx_irq_shutdown(gdev);
	auxiliary_device_uninit(&gdev->sgx->adev);
	gdev->sgx = NULL;
}
