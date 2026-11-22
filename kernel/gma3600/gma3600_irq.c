// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel GMA 3600 (Cedar Trail) DRM/KMS Driver — Interrupt Handling & VBlank
 *
 * Copyright (C) 2026 Antigravity Project
 */

#include <linux/interrupt.h>
#include <linux/pci.h>

#include <drm/drm_vblank.h>
#include <drm/drm_probe_helper.h>

#include "gma3600_drv.h"
#include "gma3600_reg.h"

void gma3600_irq_enable_vblank(struct gma3600_device *gdev, enum gma3600_pipe pipe)
{
	unsigned long flags;
	u32 pipestat_reg = (pipe == PIPE_A) ? PIPEASTAT : PIPEBSTAT;
	u32 pipe_event = (pipe == PIPE_A) ? DE_PIPEA_EVENT : DE_PIPEB_EVENT;
	u32 val;

	spin_lock_irqsave(&gdev->irq_lock, flags);

	/* Enable VBLANK interrupt in Pipe Status register */
	val = gma3600_read(gdev, pipestat_reg);
	val |= PIPE_VBLANK_INT_EN;
	gma3600_write(gdev, pipestat_reg, val | PIPE_VBLANK_INT_STATUS);

	/* Unmask in top-level IMR and enable in IER */
	gdev->irq_mask |= pipe_event;
	gma3600_write(gdev, IMR, ~gdev->irq_mask);
	gma3600_write(gdev, IER, gdev->irq_mask | DE_MASTER_IRQ_CONTROL);

	spin_unlock_irqrestore(&gdev->irq_lock, flags);
}

void gma3600_irq_disable_vblank(struct gma3600_device *gdev, enum gma3600_pipe pipe)
{
	unsigned long flags;
	u32 pipestat_reg = (pipe == PIPE_A) ? PIPEASTAT : PIPEBSTAT;
	u32 val;

	spin_lock_irqsave(&gdev->irq_lock, flags);

	val = gma3600_read(gdev, pipestat_reg);
	val &= ~PIPE_VBLANK_INT_EN;
	gma3600_write(gdev, pipestat_reg, val);

	spin_unlock_irqrestore(&gdev->irq_lock, flags);
}

static irqreturn_t gma3600_irq_handler(int irq, void *arg)
{
	struct gma3600_device *gdev = arg;
	u32 iir, pipeastat, pipebstat;
	unsigned long flags;
	irqreturn_t ret = IRQ_NONE;

	spin_lock_irqsave(&gdev->irq_lock, flags);

	iir = gma3600_read(gdev, IIR);
	if (!iir) {
		spin_unlock_irqrestore(&gdev->irq_lock, flags);
		return IRQ_NONE;
	}

	/* Write 1 to clear acknowledged interrupts */
	gma3600_write(gdev, IIR, iir);
	(void)gma3600_read(gdev, IIR);

	spin_unlock_irqrestore(&gdev->irq_lock, flags);

	/* Pipe A events (VBLANK) */
	if (iir & DE_PIPEA_EVENT) {
		pipeastat = gma3600_read(gdev, PIPEASTAT);
		if (pipeastat & PIPE_VBLANK_INT_STATUS) {
			/* Clear pipe status event */
			gma3600_write(gdev, PIPEASTAT, pipeastat | PIPE_VBLANK_INT_STATUS);
			drm_crtc_handle_vblank(&gdev->crtcs[PIPE_A].base);
			ret = IRQ_HANDLED;
		}
	}

	/* Pipe B events (VBLANK) */
	if (iir & DE_PIPEB_EVENT) {
		pipebstat = gma3600_read(gdev, PIPEBSTAT);
		if (pipebstat & PIPE_VBLANK_INT_STATUS) {
			/* Clear pipe status event */
			gma3600_write(gdev, PIPEBSTAT, pipebstat | PIPE_VBLANK_INT_STATUS);
			drm_crtc_handle_vblank(&gdev->crtcs[PIPE_B].base);
			ret = IRQ_HANDLED;
		}
	}

	/* Hotplug interrupt */
	if (iir & DE_HOTPLUG) {
		drm_kms_helper_hotplug_event(&gdev->drm);
		ret = IRQ_HANDLED;
	}

	return ret;
}

int gma3600_irq_install(struct gma3600_device *gdev)
{
	struct pci_dev *pdev = gdev->pdev;
	int ret;

	/* Disable all interrupts prior to installation */
	gma3600_write(gdev, IER, 0);
	gma3600_write(gdev, IMR, 0xFFFFFFFF);
	gma3600_write(gdev, IIR, gma3600_read(gdev, IIR));

	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_ALL_TYPES);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to allocate IRQ vector: %d\n", ret);
		return ret;
	}

	ret = request_irq(pdev->irq, gma3600_irq_handler, IRQF_SHARED,
			  DRIVER_NAME, gdev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request IRQ %d: %d\n", pdev->irq, ret);
		pci_free_irq_vectors(pdev);
		return ret;
	}

	/* Enable hotplug and master control */
	gdev->irq_mask = DE_HOTPLUG;
	gma3600_write(gdev, IMR, ~gdev->irq_mask);
	gma3600_write(gdev, IER, gdev->irq_mask | DE_MASTER_IRQ_CONTROL);

	return 0;
}

void gma3600_irq_uninstall(struct gma3600_device *gdev)
{
	struct pci_dev *pdev = gdev->pdev;

	/* Disable all interrupts in hardware */
	gma3600_write(gdev, IER, 0);
	gma3600_write(gdev, IMR, 0xFFFFFFFF);
	gma3600_write(gdev, IIR, gma3600_read(gdev, IIR));
	(void)gma3600_read(gdev, IIR);

	free_irq(pdev->irq, gdev);
	pci_free_irq_vectors(pdev);
}
