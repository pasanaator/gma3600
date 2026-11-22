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

#define GMA3600_GTT_OFFSET_BAR0		0x80000		/* 512KB offset in BAR 0 */
#define GMA3600_PTE_SIZE		4		/* 32-bit PTE */

void gma3600_gtt_write_pte(struct gma3600_device *gdev, u32 entry, dma_addr_t addr)
{
	u32 pte_val = (lower_32_bits(addr) & ~0xFFF) | GMA3600_PTE_VALID;
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
	size_t bitmap_size;

	mutex_init(&gtt->lock);

	/*
	 * Cedar Trail BAR 0 is 1MB. The lower 512KB is MMIO registers,
	 * and the upper 512KB is the GTT table holding 128K 32-bit PTEs.
	 * 128K * 4KB page = 512MB addressable aperture.
	 */
	if (gdev->mmio_size > GMA3600_GTT_OFFSET_BAR0) {
		gtt->gtt_map = gdev->mmio + GMA3600_GTT_OFFSET_BAR0;
		gtt->gtt_total_entries = (gdev->mmio_size - GMA3600_GTT_OFFSET_BAR0) / GMA3600_PTE_SIZE;
	} else {
		/* Fallback to aperture size calculation */
		gtt->gtt_map = gdev->mmio + 0x10000;
		gtt->gtt_total_entries = gdev->aperture_size / GMA3600_GTT_PAGE_SIZE;
	}

	if (gtt->gtt_total_entries > (gdev->aperture_size / GMA3600_GTT_PAGE_SIZE))
		gtt->gtt_total_entries = gdev->aperture_size / GMA3600_GTT_PAGE_SIZE;

	dev_info(&gdev->pdev->dev, "GTT total entries: %u (maps %u MB aperture)\n",
		 gtt->gtt_total_entries,
		 (gtt->gtt_total_entries * GMA3600_GTT_PAGE_SIZE) >> 20);

	/* Stolen memory entry reservation */
	gtt->gtt_stolen_entries = gtt->stolen_size / GMA3600_GTT_PAGE_SIZE;
	if (gtt->gtt_stolen_entries > gtt->gtt_total_entries)
		gtt->gtt_stolen_entries = gtt->gtt_total_entries;

	/* Allocate allocation tracking bitmap */
	bitmap_size = BITS_TO_LONGS(gtt->gtt_total_entries) * sizeof(unsigned long);
	gtt->gtt_free_bitmap = kzalloc(bitmap_size, GFP_KERNEL);
	if (!gtt->gtt_free_bitmap)
		return -ENOMEM;

	/* Mark stolen memory region as allocated (bit set = allocated) */
	bitmap_set(gtt->gtt_free_bitmap, 0, gtt->gtt_stolen_entries);

	/* Populate stolen memory PTEs into GTT */
	for (i = 0; i < gtt->gtt_stolen_entries; i++) {
		dma_addr_t paddr = (dma_addr_t)(gtt->stolen_base + (i * GMA3600_GTT_PAGE_SIZE));
		gma3600_gtt_write_pte(gdev, i, paddr);
	}

	/* Clear remaining dynamic GTT PTEs */
	for (i = gtt->gtt_stolen_entries; i < gtt->gtt_total_entries; i++)
		gma3600_gtt_clear_pte(gdev, i);

	/* Flush write to PCIe */
	(void)ioread32(gtt->gtt_map);

	/* Enable Page Table in hardware */
	gma3600_write(gdev, PGETBL_CTL, PGETBL_ENABLED);

	return 0;
}

void gma3600_gtt_fini(struct gma3600_device *gdev)
{
	struct gma3600_gtt *gtt = &gdev->gtt;
	u32 i;

	if (!gtt->gtt_free_bitmap)
		return;

	/* Disable Page Table */
	gma3600_write(gdev, PGETBL_CTL, 0);

	/* Clear all dynamic PTEs */
	for (i = gtt->gtt_stolen_entries; i < gtt->gtt_total_entries; i++)
		gma3600_gtt_clear_pte(gdev, i);

	(void)ioread32(gtt->gtt_map);

	kfree(gtt->gtt_free_bitmap);
	gtt->gtt_free_bitmap = NULL;
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
