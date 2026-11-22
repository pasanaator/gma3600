// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel GMA 3600 (Cedar Trail) DRM/KMS Driver — Main Entry Point
 *
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm.h>
#include <linux/version.h>

#include <drm/drm_aperture.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
#include <drm/drm_fbdev_ttm.h>
#endif
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_vblank.h>
#include <drm/drm_gem.h>

#include "gma3600_drv.h"
#include "gma3600_reg.h"
#include "gma3600_sgx.h"

/*
 * PCI Device Identification Table
 * Cedar Trail integrates GMA 3600 (Atom D2500/D2550/D2700/N2600/N2800)
 * PCI device IDs 0x0BE0 - 0x0BEF.
 */
static const struct pci_device_id gma3600_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_CDV_0) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_CDV_1) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_CDV_2) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_CDV_3) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_CDV_4) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_CDV_5) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_CDV_6) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_CDV_7) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_CDV_8) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_CDV_9) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_CDV_A) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_CDV_B) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_CDV_C) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_CDV_D) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_CDV_E) },
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_CDV_F) },
	{ }
};
MODULE_DEVICE_TABLE(pci, gma3600_pci_ids);

DEFINE_DRM_GEM_FOPS(gma3600_fops);

int gma3600_refclk = 0;
module_param_named(refclk, gma3600_refclk, int, 0444);
MODULE_PARM_DESC(refclk, "Override DPLL reference clock in kHz (27000 or 96000, 0=auto-detect)");

static const struct drm_driver gma3600_drm_driver = {
	.driver_features       = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.fops                  = &gma3600_fops,
	.name                  = DRIVER_NAME,
	.desc                  = DRIVER_DESC,
	.major                 = DRIVER_MAJOR,
	.minor                 = DRIVER_MINOR,
	.patchlevel            = DRIVER_PATCHLEVEL,
#ifdef DRM_FBDEV_TTM_DRIVER_OPS
	DRM_FBDEV_TTM_DRIVER_OPS,
#endif
	.dumb_create           = gma3600_gem_dumb_create,
	.dumb_map_offset       = drm_gem_dumb_map_offset,
};

static int gma3600_pci_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct gma3600_device *gdev;
	int ret;

	if (gma3600_refclk != 0 && gma3600_refclk != 27000 &&
	    gma3600_refclk != 96000)
		return -EINVAL;

	ret = drm_aperture_remove_conflicting_pci_framebuffers(pdev,
						     &gma3600_drm_driver);
	if (ret)
		return ret;

	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "pci_enable_device failed: %d\n", ret);
		return ret;
	}

	pci_set_master(pdev);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "dma_set_mask_and_coherent(32) failed: %d\n", ret);
		goto err_disable_pci;
	}

	gdev = devm_drm_dev_alloc(&pdev->dev, &gma3600_drm_driver,
				  struct gma3600_device, drm);
	if (IS_ERR(gdev)) {
		ret = PTR_ERR(gdev);
		dev_err(&pdev->dev, "devm_drm_dev_alloc failed: %d\n", ret);
		goto err_disable_pci;
	}

	gdev->pdev = pdev;
	pci_set_drvdata(pdev, gdev);

	/* Locate Intel GMCH host bridge (PCI 0:0.0) for stolen memory size */
	gdev->bridge = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(0, 0));
	if (!gdev->bridge)
		dev_warn(&pdev->dev, "Host bridge (0:0.0) not found, fallback memory detection\n");

	/* Initialize hardware and memory apertures */
	ret = gma3600_device_init(gdev);
	if (ret) {
		dev_err(&pdev->dev, "gma3600_device_init failed: %d\n", ret);
		goto err_put_bridge;
	}

	/* Initialize GEM subsystem */
	ret = gma3600_gem_init(gdev);
	if (ret) {
		dev_err(&pdev->dev, "gma3600_gem_init failed: %d\n", ret);
		goto err_fini_device;
	}

	/* Initialize GMBUS I2C / DDC */
	ret = gma3600_i2c_init(gdev);
	if (ret) {
		dev_err(&pdev->dev, "gma3600_i2c_init failed: %d\n", ret);
		goto err_fini_device;
	}

	/* Initialize KMS Display (CRTCs, planes, encoders, connectors) */
	ret = gma3600_display_init(gdev);
	if (ret) {
		dev_err(&pdev->dev, "gma3600_display_init failed: %d\n", ret);
		goto err_fini_i2c;
	}

	/* Install IRQ handler */
	ret = gma3600_irq_install(gdev);
	if (ret) {
		dev_err(&pdev->dev, "gma3600_irq_install failed: %d\n", ret);
		goto err_fini_display;
	}

	/* Initialize Backlight device */
	ret = gma3600_backlight_init(gdev);
	if (ret)
		dev_warn(&pdev->dev, "gma3600_backlight_init failed: %d (continuing)\n", ret);

	/* Register DRM device with the subsystem */
	ret = drm_dev_register(&gdev->drm, 0);
	if (ret) {
		dev_err(&pdev->dev, "drm_dev_register failed: %d\n", ret);
		goto err_uninstall_irq;
	}

	drm_kms_helper_poll_init(&gdev->drm);

	/* Setup generic fbdev emulation for Linux VT / console */
	ret = gma3600_fbdev_init(gdev);
	if (ret)
		dev_warn(&pdev->dev, "gma3600_fbdev_init failed: %d (continuing)\n", ret);

	dev_info(&pdev->dev, "Intel GMA 3600 (Cedar Trail) initialized successfully\n");
	/* SGX discovery is optional; its failure must not tear down working KMS. */
	ret = gma3600_sgx_host_init(gdev);
	if (ret)
		dev_warn(&pdev->dev, "SGX companion unavailable: %d\n", ret);
	return 0;

err_uninstall_irq:
	gma3600_backlight_fini(gdev);
	gma3600_irq_uninstall(gdev);
err_fini_display:
	gma3600_display_fini(gdev);
err_fini_i2c:
	gma3600_i2c_fini(gdev);
err_fini_device:
	gma3600_device_fini(gdev);
err_put_bridge:
	if (gdev->bridge)
		pci_dev_put(gdev->bridge);
err_disable_pci:
	pci_disable_device(pdev);
	return ret;
}

static void gma3600_pci_remove(struct pci_dev *pdev)
{
	struct gma3600_device *gdev = pci_get_drvdata(pdev);

	if (!gdev)
		return;

	/* Unbind the child before releasing any of its parent's resources. */
	gma3600_sgx_host_fini(gdev);
	drm_kms_helper_poll_fini(&gdev->drm);
	drm_dev_unregister(&gdev->drm);
	gma3600_backlight_fini(gdev);
	gma3600_display_fini(gdev);
	gma3600_irq_uninstall(gdev);
	gma3600_i2c_fini(gdev);
	gma3600_device_fini(gdev);

	if (gdev->bridge) {
		pci_dev_put(gdev->bridge);
		gdev->bridge = NULL;
	}

	pci_disable_device(pdev);
}

static void gma3600_pci_shutdown(struct pci_dev *pdev)
{
	struct gma3600_device *gdev = pci_get_drvdata(pdev);

	if (gdev)
		drm_atomic_helper_shutdown(&gdev->drm);
}

static const struct dev_pm_ops gma3600_pm_ops = {
	.suspend  = gma3600_pm_suspend,
	.resume   = gma3600_pm_resume,
	.freeze   = gma3600_pm_freeze,
	.thaw     = gma3600_pm_thaw,
	.poweroff = gma3600_pm_suspend,
	.restore  = gma3600_pm_resume,
};

static struct pci_driver gma3600_pci_driver = {
	.name     = DRIVER_NAME,
	.id_table = gma3600_pci_ids,
	.probe    = gma3600_pci_probe,
	.remove   = gma3600_pci_remove,
	.shutdown = gma3600_pci_shutdown,
	.driver   = {
		.pm = &gma3600_pm_ops,
	},
};

module_pci_driver(gma3600_pci_driver);

MODULE_AUTHOR("Henri Proos");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
#ifdef GMA3600_SGX_HOST
MODULE_VERSION("1.0.4-sgx-host2");
#else
MODULE_VERSION("1.0.4");
#endif

#ifdef MODULE_IMPORT_NS
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
MODULE_IMPORT_NS("DMA_BUF");
#else
MODULE_IMPORT_NS(DMA_BUF);
#endif
#endif
