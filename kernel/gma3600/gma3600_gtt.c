// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel GMA 3600 (Cedar Trail) DRM/KMS Driver — GTT Management
 *
 * Copyright (C) 2026 Antigravity Project
 */

#include <linux/pci.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/bitmap.h>

#include "gma3600_drv.h"
#include "gma3600_reg.h"

#define GMA3600_PTE_SIZE		4		/* 32-bit PTE */

void gma3600_gtt_write_pte(struct gma3600_device *gdev, u32 entry, dma_addr_t addr)
{
	u32 pte_val = (lower_32_bits(addr) & ~0xFFF) | GMA3600_PTE_VALID | GMA3600_PTE_CACHED;
	void __iomem *pte_addr = gdev->gtt.gtt_map + (entry * GMA3600_PTE_SIZE);

	iowrite32(pte_val, pte_addr);
}

void gma3600_gtt_clear_pte(struct gma3600_device *gdev, u32 entry)
{
	void __iomem *pte_addr = gdev->gtt.gtt_map + (entry * GMA3600_PTE_SIZE);

	iowrite32(0, pte_addr);
}

int gma3600_gtt_init(struct gma3600_device *gdev)
{
	struct gma3600_gtt *gtt = &gdev->gtt;
	u32 i;
	u16 gmch_ctrl;
	int ret;
	size_t bitmap_size;

	mutex_init(&gtt->lock);

	/* This is the IGD's PCI control word, not the host bridge's GMS.
	 * Cedarview needs both GMCH and PGETBL enabled for display fetches.
	 */
	ret = pci_read_config_word(gdev->pdev, GMA3600_GMCH_CTRL,
				   &gtt->saved_gmch_ctrl);
	if (ret) {
		dev_err(&gdev->pdev->dev, "GTT: PCI GMCH read failed (%d)\n", ret);
		return pcibios_err_to_errno(ret);
	}

	/* Cedarview's GTT is at the physical address in PGETBL_CTL,
	 * not a guessed offset inside the display-register BAR. BAR 3 can
	 * omit its size on CDV; Linux's gma500 uses 64 table pages then.
	 */
	gtt->saved_pgetbl_ctl = gma3600_read(gdev, PGETBL_CTL);
	gtt->gtt_phys = gtt->saved_pgetbl_ctl & ~0xfffU;
	if (!gtt->gtt_phys)
		return -ENODEV;
	gtt->gtt_total_entries = (pci_resource_len(gdev->pdev, 3) ?: 64 * PAGE_SIZE) /
				GMA3600_PTE_SIZE;
	gtt->gtt_map = ioremap(gtt->gtt_phys,
			       gtt->gtt_total_entries * GMA3600_PTE_SIZE);
	if (!gtt->gtt_map)
		return -ENOMEM;

	if (gtt->gtt_total_entries > (gdev->aperture_size / GMA3600_GTT_PAGE_SIZE))
		gtt->gtt_total_entries = gdev->aperture_size / GMA3600_GTT_PAGE_SIZE;

	dev_info(&gdev->pdev->dev, "GTT total entries: %u (maps %u MB aperture)\n",
		 gtt->gtt_total_entries,
		 (gtt->gtt_total_entries * GMA3600_GTT_PAGE_SIZE) >> 20);

	/* All GEM objects use system RAM; no guessed stolen-memory mapping. */
	gtt->gtt_stolen_entries = 0;

	/* Allocate allocation tracking bitmap */
	bitmap_size = BITS_TO_LONGS(gtt->gtt_total_entries) * sizeof(unsigned long);
	gtt->gtt_free_bitmap = kzalloc(bitmap_size, GFP_KERNEL);
	if (!gtt->gtt_free_bitmap) {
		iounmap(gtt->gtt_map);
		gtt->gtt_map = NULL;
		return -ENOMEM;
	}

	/* Clear remaining dynamic GTT PTEs */
	for (i = gtt->gtt_stolen_entries; i < gtt->gtt_total_entries; i++)
		gma3600_gtt_clear_pte(gdev, i);

	/* Flush write to PCIe */
	(void)ioread32(gtt->gtt_map);

	ret = pci_write_config_word(gdev->pdev, GMA3600_GMCH_CTRL,
				    gtt->saved_gmch_ctrl | GMA3600_GMCH_ENABLED);
	if (ret) {
		dev_err(&gdev->pdev->dev, "GTT: PCI GMCH write failed (%d)\n", ret);
		ret = pcibios_err_to_errno(ret);
		goto err_free;
	}
	ret = pci_read_config_word(gdev->pdev, GMA3600_GMCH_CTRL, &gmch_ctrl);
	/* Linux gma500 checks PCI write status, not whether this legacy
	 * bit reads back set. A strict readback test rejected the D2500HN.
	 * Keep readback diagnostic; a successful PCI write is sufficient.
	 */
	if (ret) {
		dev_warn(&gdev->pdev->dev,
			 "GTT: GMCH readback unavailable (%d); PCI write succeeded\n", ret);
		gmch_ctrl = gtt->saved_gmch_ctrl;
	} else if (!(gmch_ctrl & GMA3600_GMCH_ENABLED)) {
		dev_warn(&gdev->pdev->dev,
			 "GTT: GMCH enable bit reads clear (%04x -> %04x); continuing as gma500 does\n",
			 gtt->saved_gmch_ctrl, gmch_ctrl);
	}
	gma3600_write_post(gdev, PGETBL_CTL,
			    gtt->saved_pgetbl_ctl | PGETBL_ENABLED);
	dev_info(&gdev->pdev->dev,
		 "GTT: GMCH %04x -> %04x, PGETBL=%08x, table=%pa\n",
		 gtt->saved_gmch_ctrl, gmch_ctrl, gma3600_read(gdev, PGETBL_CTL),
		 &gtt->gtt_phys);

	return 0;
err_free:
	kfree(gtt->gtt_free_bitmap);
	gtt->gtt_free_bitmap = NULL;
	iounmap(gtt->gtt_map);
	gtt->gtt_map = NULL;
	return ret;
}

void gma3600_gtt_fini(struct gma3600_device *gdev)
{
	struct gma3600_gtt *gtt = &gdev->gtt;
	u32 i;

	if (!gtt->gtt_free_bitmap)
		return;

	/* Restore firmware control bits; do not zero the table address. */
	gma3600_write_post(gdev, PGETBL_CTL, gtt->saved_pgetbl_ctl);

	/* Clear all dynamic PTEs */
	for (i = gtt->gtt_stolen_entries; i < gtt->gtt_total_entries; i++)
		gma3600_gtt_clear_pte(gdev, i);

	(void)ioread32(gtt->gtt_map);
	pci_write_config_word(gdev->pdev, GMA3600_GMCH_CTRL, gtt->saved_gmch_ctrl);

	kfree(gtt->gtt_free_bitmap);
	gtt->gtt_free_bitmap = NULL;
	iounmap(gtt->gtt_map);
	gtt->gtt_map = NULL;
}

int gma3600_gtt_insert(struct gma3600_device *gdev, struct gma3600_gem_object *obj)
{
	struct gma3600_gtt *gtt = &gdev->gtt;
	unsigned long start_entry;
	u32 i;

	if (obj->in_gtt)
		return 0;

	mutex_lock(&gtt->lock);

	/* Find a contiguous range of free PTEs */
	start_entry = bitmap_find_next_zero_area(gtt->gtt_free_bitmap,
						gtt->gtt_total_entries,
						gtt->gtt_stolen_entries,
						obj->gtt_num_pages, 0);

	if (start_entry >= gtt->gtt_total_entries) {
		mutex_unlock(&gtt->lock);
		dev_err(&gdev->pdev->dev, "Out of GTT aperture space for %u pages\n",
			obj->gtt_num_pages);
		return -ENOSPC;
	}

	bitmap_set(gtt->gtt_free_bitmap, start_entry, obj->gtt_num_pages);

	/* Write DMA pages into the hardware GTT */
	for (i = 0; i < obj->num_pages; i++)
		gma3600_gtt_write_pte(gdev, start_entry + i, obj->dma_addrs[i]);

	/* Ensure memory postings are flushed to GPU */
	wmb();
	(void)ioread32(gtt->gtt_map + (start_entry * GMA3600_PTE_SIZE));

	obj->gtt_offset = start_entry * GMA3600_GTT_PAGE_SIZE;
	obj->in_gtt = true;

	mutex_unlock(&gtt->lock);
	return 0;
}

void gma3600_gtt_remove(struct gma3600_device *gdev, struct gma3600_gem_object *obj)
{
	struct gma3600_gtt *gtt = &gdev->gtt;
	u32 start_entry, i;

	if (!obj->in_gtt || obj->stolen)
		return;

	/* DRM objects can outlive PCI removal. Hardware is already stopped. */
	if (!gtt->gtt_free_bitmap) {
		obj->in_gtt = false;
		return;
	}

	start_entry = obj->gtt_offset / GMA3600_GTT_PAGE_SIZE;

	mutex_lock(&gtt->lock);

	for (i = 0; i < obj->gtt_num_pages; i++)
		gma3600_gtt_clear_pte(gdev, start_entry + i);

	bitmap_clear(gtt->gtt_free_bitmap, start_entry, obj->gtt_num_pages);

	wmb();
	(void)ioread32(gtt->gtt_map + (start_entry * GMA3600_PTE_SIZE));

	obj->in_gtt = false;
	obj->gtt_offset = 0;

	mutex_unlock(&gtt->lock);
}
