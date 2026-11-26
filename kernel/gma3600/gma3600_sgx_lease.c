// SPDX-License-Identifier: GPL-2.0-only
/* Parent resource leases; no SGX MMIO, PCI power changes or GPU execution. */
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <drm/drm_device.h>
#include "gma3600_drv.h"
#include "gma3600_sgx.h"

void gma3600_sgx_lifecycle_init(struct gma3600_device *gdev)
{
	mutex_init(&gdev->sgx_lifecycle_mutex);
	gdev->sgx_suspending = gdev->sgx_removing = false;
	gdev->sgx_lease_owner = NULL;
	gdev->sgx_lease_sealed = false;
}

int gma3600_sgx_suspend_begin(struct gma3600_device *gdev)
{
	int ret = 0;
	mutex_lock(&gdev->sgx_lifecycle_mutex);
	if (gdev->sgx_removing) ret = -ENODEV;
	else if (gdev->sgx_lease_owner || gdev->sgx_suspending) ret = -EBUSY;
	else gdev->sgx_suspending = true;
	mutex_unlock(&gdev->sgx_lifecycle_mutex);
	return ret;
}
void gma3600_sgx_suspend_end(struct gma3600_device *gdev)
{
	mutex_lock(&gdev->sgx_lifecycle_mutex);
	gdev->sgx_suspending = false;
	mutex_unlock(&gdev->sgx_lifecycle_mutex);
}
void gma3600_sgx_remove_begin(struct gma3600_device *gdev)
{
	mutex_lock(&gdev->sgx_lifecycle_mutex);
	gdev->sgx_removing = true;
	mutex_unlock(&gdev->sgx_lifecycle_mutex);
}
static int lease_acquire(struct gma3600_sgx_device *sgx, void *identity)
{
	struct gma3600_device *gdev = sgx->host;
	int ret = 0;
	if (!identity) return -EINVAL;
	mutex_lock(&gdev->sgx_lifecycle_mutex);
	mutex_lock(&gdev->sgx_irq_mutex);
	if (gdev->sgx_removing || gdev->sgx_irq_closing) ret = -ENODEV;
	else if (gdev->sgx_suspending || gdev->suspended || gdev->sgx_lease_owner || gdev->sgx_irq_handler)
		ret = -EBUSY;
	else if (pci_channel_offline(sgx->pdev) || sgx->pdev->current_state != PCI_D0) ret = -EAGAIN;
	else {
		int power = pm_runtime_get_if_active(&sgx->pdev->dev);
		/* Never resume hardware for a CPU lease. When runtime PM is disabled
		 * by this driver, get_noresume also prevents a later enable from
		 * suspending the device underneath the retained owner. */
		if (power == 0 || (power < 0 && pm_runtime_enabled(&sgx->pdev->dev))) {
			ret = -EAGAIN;
			goto unlock;
		}
		if (power < 0) pm_runtime_get_noresume(&sgx->pdev->dev);
		/* gdev must survive forced PCI unbind until the CPU lease is retired.
		 * No MMIO or device wake is performed while obtaining these refs. */
		drm_dev_get(&gdev->drm);
		pci_dev_get(sgx->pdev);
		gdev->sgx_lease_owner = identity;
	}
unlock:
	mutex_unlock(&gdev->sgx_irq_mutex);
	mutex_unlock(&gdev->sgx_lifecycle_mutex);
	return ret;
}
static int lease_release(struct gma3600_sgx_device *sgx, void *identity)
{
	struct gma3600_device *gdev = sgx->host;
	int ret = 0;
	mutex_lock(&gdev->sgx_lifecycle_mutex);
	if (!identity || gdev->sgx_lease_owner != identity) ret = -ENOENT;
	else if (gdev->sgx_lease_sealed) ret = -EBUSY;
	else gdev->sgx_lease_owner = NULL;
	mutex_unlock(&gdev->sgx_lifecycle_mutex);
	if (!ret) {
		pm_runtime_put_noidle(&sgx->pdev->dev);
		pci_dev_put(sgx->pdev);
		drm_dev_put(&gdev->drm); /* Last access to gdev; storage may be released. */
	}
	return ret;
}
static int lease_seal(struct gma3600_sgx_device *sgx, void *identity)
{
	struct gma3600_device *gdev = sgx->host;
	int ret = 0;
	mutex_lock(&gdev->sgx_lifecycle_mutex);
	if (!identity || gdev->sgx_lease_owner != identity) ret = -ENOENT;
	else if (gdev->sgx_removing || gdev->sgx_suspending) ret = -ENODEV;
	else if (sgx->pdev->vendor != 0x8086 || sgx->pdev->device != 0x0be1) ret = -ENODEV;
	else if (pci_channel_offline(sgx->pdev) || sgx->pdev->current_state != PCI_D0) ret = -EAGAIN;
	else if (gdev->sgx_lease_sealed) ret = -EALREADY;
	else gdev->sgx_lease_sealed = true;
	mutex_unlock(&gdev->sgx_lifecycle_mutex);
	return ret;
}
const struct gma3600_sgx_lease_ops gma3600_sgx_lease_ops = {
	.owner = THIS_MODULE, .acquire = lease_acquire, .release = lease_release, .seal = lease_seal,
};
