// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel GMA 3600 (Cedar Trail) DRM/KMS Driver — Device Hardware Setup
 *
 * Copyright (C) 2026 Antigravity Project
 */

#include <linux/pci.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/dmi.h>

#include "gma3600_drv.h"
#include "gma3600_reg.h"

int gma3600_device_init(struct gma3600_device *gdev)
{
	struct pci_dev *pdev = gdev->pdev;
	u32 bsm = 0;
	int ret;

	/*
	 * BAR 0: display MMIO registers; the GTT is mapped separately.
	 */
	gdev->mmio_phys = pci_resource_start(pdev, 0);
	gdev->mmio_size = pci_resource_len(pdev, 0);
	if (!gdev->mmio_phys || !gdev->mmio_size) {
		dev_err(&pdev->dev, "Invalid BAR 0 configuration\n");
		return -ENODEV;
	}

	gdev->mmio = pci_iomap(pdev, 0, 0);
	if (!gdev->mmio) {
		dev_err(&pdev->dev, "Failed to map BAR 0 (MMIO)\n");
		return -ENOMEM;
	}

	/*
	 * BAR 2: Graphics Memory Aperture (Direct CPU visible aperture)
	 */
	gdev->aperture_phys = pci_resource_start(pdev, 2);
	gdev->aperture_size = pci_resource_len(pdev, 2);
	if (!gdev->aperture_phys || !gdev->aperture_size) {
		dev_warn(&pdev->dev, "BAR 2 aperture size invalid, using default 256MB\n");
		gdev->aperture_size = 256 * 1024 * 1024;
	}

	dev_info(&pdev->dev, "MMIO at 0x%pap (%llu KB), Aperture at 0x%pap (%llu MB)\n",
		 &gdev->mmio_phys, (u64)(gdev->mmio_size >> 10),
		 &gdev->aperture_phys, (u64)(gdev->aperture_size >> 20));

	/* Disable legacy VGA hardware plane and display emulation */
	gma3600_write(gdev, VGACNTRL, VGA_DISP_DISABLE);

	/*
	 * Detect Stolen Memory (Pre-allocated BIOS frame buffer memory)
	 * Read Base of Stolen Memory (BSM) from PCI config offset 0x5C.
	 */
	pci_read_config_dword(pdev, GMA3600_PCI_BSM, &bsm);
	gdev->gtt.stolen_base = bsm & ~0xFFF;

	/* Buffers are allocated from system RAM, not firmware stolen memory. */
	gdev->gtt.stolen_size = 0;

	/* Read the BIOS clock before shutting down its pipes. A VBT signature
	 * alone cannot identify a board's reference crystal or panel wiring.
	 */
	{
		u32 n_vco, n;

		gdev->has_lvds = !!(gma3600_read(gdev, LVDS) & LVDS_PORT_EN);
		gdev->dplla_96mhz = gdev->has_lvds;
		if (!gma3600_sb_read(gdev, SB_N_VCO(PIPE_A), &n_vco)) {
			n = (n_vco & SB_N_DIVIDER_MASK) >> SB_N_DIVIDER_SHIFT;
			gdev->dplla_96mhz = n >= 2 && n <= 6;
		}
		/* Desktop Cedarview board with a 27 MHz display reference. */
		if (dmi_match(DMI_BOARD_NAME, "D2500HN"))
			gdev->dplla_96mhz = false;
		if (gma3600_refclk)
			gdev->dplla_96mhz = gma3600_refclk == 96000;
		gdev->lvds_ssc_freq = GMA3600_REF_CLK_96;
		gdev->lvds_use_ssc = false;
		dev_info(&pdev->dev, "Display reference %d kHz, LVDS %s\n",
			 gdev->dplla_96mhz ? 96000 : 27000,
			 gdev->has_lvds ? "present" : "absent");
	}

	/* DRM starts with disabled software state. Quiesce the firmware's
	 * scanout before changing its page tables or reprogramming a live PLL.
	 */
	gma3600_write_post(gdev, ADPA, ADPA_VSYNC_CNTL_DISABLE |
					 ADPA_HSYNC_CNTL_DISABLE);
	gma3600_write_post(gdev, HDMIB, gma3600_read(gdev, HDMIB) & ~HDMI_PORT_EN);
	gma3600_write_post(gdev, LVDS, gma3600_read(gdev, LVDS) & ~LVDS_PORT_EN);
	{
		enum gma3600_pipe pipe;
		u32 val;

		for (pipe = PIPE_A; pipe <= PIPE_B; pipe++) {
			gma3600_write(gdev, CURCNTR(pipe), CURSOR_MODE_DISABLE);
			gma3600_write(gdev, DSPCNTR(pipe), 0);
			gma3600_write_post(gdev, DSPSURF(pipe), 0);
			gma3600_write_post(gdev, PIPECONF(pipe),
					    gma3600_read(gdev, PIPECONF(pipe)) & ~PIPE_ENABLE);
			ret = read_poll_timeout(gma3600_read, val,
					       !(val & PIPE_STATE_ENABLED),
					       100, 100000, false, gdev, PIPECONF(pipe));
			if (ret) {
				dev_err(&pdev->dev, "Firmware pipe %c did not stop\n", 'A' + pipe);
				goto err_unmap;
			}
			gma3600_dpll_disable(gdev, pipe);
		}
	}

	ret = gma3600_gtt_init(gdev);
	if (ret)
		goto err_unmap;

	spin_lock_init(&gdev->irq_lock);

	return 0;

err_unmap:
	pci_iounmap(pdev, gdev->mmio);
	gdev->mmio = NULL;
	return ret;
}

void gma3600_device_fini(struct gma3600_device *gdev)
{
	struct pci_dev *pdev = gdev->pdev;

	gma3600_gtt_fini(gdev);

	if (gdev->mmio) {
		pci_iounmap(pdev, gdev->mmio);
		gdev->mmio = NULL;
	}
}
