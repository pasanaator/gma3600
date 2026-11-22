// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel GMA 3600 (Cedar Trail) DRM/KMS Driver — Device Hardware Setup
 *
 * Copyright (C) 2026 Antigravity Project
 */

#include <linux/pci.h>
#include <linux/io.h>

#include "gma3600_drv.h"
#include "gma3600_reg.h"

int gma3600_device_init(struct gma3600_device *gdev)
{
	struct pci_dev *pdev = gdev->pdev;
	u32 bsm = 0;
	u16 gmch_ctrl = 0;
	u8 gms;
	int ret;

	/*
	 * BAR 0: GTTMMADR (MMIO registers 512KB + GTT entries)
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

	/* Determine stolen memory size from GMCH host bridge (PCI 0:0.0) */
	gdev->gtt.stolen_size = 0;
	if (gdev->bridge) {
		pci_read_config_word(gdev->bridge, GMA3600_GMCH_CTRL, &gmch_ctrl);
		gms = (gmch_ctrl & GMCH_GMS_MASK) >> GMCH_GMS_SHIFT;
		switch (gms) {
		case 0x1:
			gdev->gtt.stolen_size = 32 * 1024 * 1024;
			break;
		case 0x2:
			gdev->gtt.stolen_size = 64 * 1024 * 1024;
			break;
		case 0x3:
			gdev->gtt.stolen_size = 128 * 1024 * 1024;
			break;
		case 0x4:
			gdev->gtt.stolen_size = 256 * 1024 * 1024;
			break;
		default:
			dev_info(&pdev->dev, "Unknown GMS encoding 0x%x, probing fallback\n", gms);
			break;
		}
	}

	if (!gdev->gtt.stolen_size)
		gdev->gtt.stolen_size = 32 * 1024 * 1024; /* Standard 32MB fallback */

	dev_info(&pdev->dev, "Stolen memory: base 0x%08x, size %u MB\n",
		 gdev->gtt.stolen_base, gdev->gtt.stolen_size >> 20);

	/* Initialize Graphics Translation Table (GTT) */
	ret = gma3600_gtt_init(gdev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to initialize GTT: %d\n", ret);
		pci_iounmap(pdev, gdev->mmio);
		return ret;
	}

	/* Detect display PLL reference clock (27 MHz vs 96 MHz) and LVDS */
	{
		bool vbt_found = false;
		size_t rom_size = 0;
		void __iomem *rom = pci_map_rom(pdev, &rom_size);

		gdev->dplla_96mhz = false;
		gdev->has_lvds = false;

		/* 1. Try reading VBT from PCI Expansion ROM */
		if (rom && rom_size >= 64) {
			size_t i;

			for (i = 0; i + 64 <= rom_size; i++) {
				if (readb(rom + i) == '$' &&
				    readb(rom + i + 1) == 'V' &&
				    readb(rom + i + 2) == 'B' &&
				    readb(rom + i + 3) == 'T') {
					u16 bdb_offset = readw(rom + i + 24);

					if (i + bdb_offset + 32 <= rom_size) {
						void __iomem *bdb = rom + i + bdb_offset;
						u16 bdb_size = readw(bdb + 18);
						u16 hdr_size = readw(bdb + 16);
						u16 idx = hdr_size;

						while (idx + 3 < bdb_size && i + bdb_offset + idx + 3 <= rom_size) {
							u8 sec_id = readb(bdb + idx);
							u16 sec_sz = readw(bdb + idx + 1);

							if (sec_id == 12 /* BDB_DRIVER_FEATURES */ && sec_sz >= 4) {
								u8 primary_lfp_id = readb(bdb + idx + 4);
								u8 lvds_cfg = readb(bdb + idx + 6) & 0x3;

								if (primary_lfp_id)
									gdev->dplla_96mhz = true;
								if (lvds_cfg != 0)
									gdev->has_lvds = true;
								vbt_found = true;
								break;
							}
							idx += 3 + sec_sz;
						}
					}
					break;
				}
			}
			pci_unmap_rom(pdev, rom);
		}

		/* 2. Fallback: inspect initial DPLL sideband configuration set by BIOS */
		if (!vbt_found) {
			u32 init_n_vco = 0;
			if (gma3600_sb_read(gdev, SB_N_VCO(PIPE_A), &init_n_vco) == 0) {
				u32 n = (init_n_vco & SB_N_DIVIDER_MASK) >> SB_N_DIVIDER_SHIFT;
				if (n >= 2 && n <= 6)
					gdev->dplla_96mhz = true;  /* 96 MHz (Netbook / LVDS) */
				else
					gdev->dplla_96mhz = false; /* 27 MHz (Desktop CRT/HDMI) */
			} else {
				gdev->dplla_96mhz = false; /* Default 27 MHz for desktop */
			}

			/*
			 * Only consider LVDS present if the BIOS already enabled the port.
			 * Do NOT rely on LVDS_DETECTED alone as unpopulated headers on desktop
			 * motherboards float high and falsely trigger LVDS detection.
			 */
			gdev->has_lvds = (gma3600_read(gdev, LVDS) & LVDS_PORT_EN) != 0;
		}

		/* 3. If LVDS panel is confirmed present, reference clock is 96 MHz */
		if (gdev->has_lvds)
			gdev->dplla_96mhz = true;

		/* 4. Module parameter override if specified by user */
		if (gma3600_refclk == 27 || gma3600_refclk == 27000) {
			gdev->dplla_96mhz = false;
			gdev->has_lvds = false;
		} else if (gma3600_refclk == 96 || gma3600_refclk == 96000) {
			gdev->dplla_96mhz = true;
		}

		gdev->lvds_ssc_freq = GMA3600_REF_CLK_96;
		gdev->lvds_use_ssc = false;

		dev_info(&pdev->dev, "Display PLL reference clock: %s (%d kHz), LVDS: %s\n",
			 gdev->dplla_96mhz ? "96 MHz" : "27 MHz",
			 gdev->dplla_96mhz ? GMA3600_REF_CLK_96 : 27000,
			 gdev->has_lvds ? "present" : "not present");
	}

	spin_lock_init(&gdev->irq_lock);

	return 0;
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
