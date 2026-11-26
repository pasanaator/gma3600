// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel GMA 3600 (Cedar Trail) DRM/KMS Driver — Power Management
 *
 * Copyright (C) 2026 Antigravity Project
 */

#include <linux/pci.h>
#include <linux/pm.h>

#include <drm/drm_atomic_helper.h>

#include "gma3600_drv.h"
#include "gma3600_reg.h"
#include "gma3600_sgx.h"

int gma3600_pm_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct gma3600_device *gdev = pci_get_drvdata(pdev);
	int ret;

	if (!gdev || gdev->suspended)
		return 0;
	ret = gma3600_sgx_suspend_begin(gdev);
	if (ret)
		return ret;

	/* Save atomic state and disable all active CRTCs/encoders */
	ret = drm_mode_config_helper_suspend(&gdev->drm);
	if (ret) {
		dev_err(dev, "drm_mode_config_helper_suspend failed: %d\n", ret);
		gma3600_sgx_suspend_end(gdev);
		return ret;
	}

	/* Save hardware register state */
	gdev->saveVGACNTRL   = gma3600_read(gdev, VGACNTRL);
	gdev->saveDSPACNTR   = gma3600_read(gdev, DSPACNTR);
	gdev->saveDSPBCNTR   = gma3600_read(gdev, DSPBCNTR);
	gdev->savePIPEACONF  = gma3600_read(gdev, PIPEACONF);
	gdev->savePIPEBCONF  = gma3600_read(gdev, PIPEBCONF);
	gdev->saveDPLL_A     = gma3600_read(gdev, DPLL_A);
	gdev->saveDPLL_B     = gma3600_read(gdev, DPLL_B);
	gdev->saveFPA0       = gma3600_read(gdev, FPA0);
	gdev->saveFPA1       = gma3600_read(gdev, FPA1);
	gdev->saveFPB0       = gma3600_read(gdev, FPB0);
	gdev->saveFPB1       = gma3600_read(gdev, FPB1);
	gdev->saveADPA       = gma3600_read(gdev, ADPA);
	gdev->saveLVDS       = gma3600_read(gdev, LVDS);
	gdev->saveHDMIB      = gma3600_read(gdev, HDMIB);
	gdev->saveBLC_PWM_CTL  = gma3600_read(gdev, BLC_PWM_CTL);
	gdev->saveBLC_PWM_CTL2 = gma3600_read(gdev, BLC_PWM_CTL2);

	gdev->suspended = true;

	pci_save_state(pdev);
	pci_set_power_state(pdev, PCI_D3hot);

	return 0;
}

int gma3600_pm_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct gma3600_device *gdev = pci_get_drvdata(pdev);
	int ret;

	if (!gdev || !gdev->suspended)
		return 0;

	pci_set_power_state(pdev, PCI_D0);
	pci_restore_state(pdev);
	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(dev, "pci_enable_device failed on resume: %d\n", ret);
		return ret;
	}
	pci_set_master(pdev);

	/* Re-enable GTT hardware address translation */
	gma3600_write_post(gdev, PGETBL_CTL,
			    gdev->gtt.saved_pgetbl_ctl | PGETBL_ENABLED);

	/* Disable legacy VGA hardware plane */
	gma3600_write(gdev, VGACNTRL, VGA_DISP_DISABLE);

	/* Reset DPIO sideband configuration */
	gma3600_sb_reset(gdev);

	/* Restore Backlight control registers */
	gma3600_write(gdev, BLC_PWM_CTL, gdev->saveBLC_PWM_CTL);
	gma3600_write(gdev, BLC_PWM_CTL2, gdev->saveBLC_PWM_CTL2);

	/* Restore KMS atomic state */
	ret = drm_mode_config_helper_resume(&gdev->drm);
	if (ret)
		dev_err(dev, "drm_mode_config_helper_resume failed: %d\n", ret);

	/* Failed display restore must continue to exclude an SGX lease. */
	if (!ret) {
		gdev->suspended = false;
		gma3600_sgx_suspend_end(gdev);
	}
	return ret;
}

int gma3600_pm_freeze(struct device *dev)
{
	return gma3600_pm_suspend(dev);
}

int gma3600_pm_thaw(struct device *dev)
{
	return gma3600_pm_resume(dev);
}
