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
#include "gma3600_sgx.h"

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
	gma3600_write(gdev, IER, gdev->irq_mask);

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

	iir = gma3600_read(gdev, IIR) & gdev->irq_mask;
	if (!iir) {
		spin_unlock_irqrestore(&gdev->irq_lock, flags);
		return IRQ_NONE;
	}

	pipeastat = gma3600_read(gdev, PIPEASTAT);
	pipebstat = gma3600_read(gdev, PIPEBSTAT);
	/* Clear the source before acknowledging the top-level identity. */
	if (iir & DE_PIPEA_EVENT)
		gma3600_write(gdev, PIPEASTAT, pipeastat);
	if (iir & DE_PIPEB_EVENT)
		gma3600_write(gdev, PIPEBSTAT, pipebstat);
#ifdef GMA3600_SGX_HOST
	/* The SGX ISR clears its own source before the parent acknowledges IIR.
	 * It cannot run under irq_lock: legacy SGX code has its own IRQ locks.
	 * synchronize_irq() in unregister protects the copied callback's lifetime.
	 */
	if (iir & GMA3600_SGX_IRQ_BIT) {
		spin_unlock_irqrestore(&gdev->irq_lock, flags);
		gma3600_sgx_irq_dispatch(gdev, iir);
		spin_lock_irqsave(&gdev->irq_lock, flags);
	}
#endif
	gma3600_write_post(gdev, IIR, iir);
	spin_unlock_irqrestore(&gdev->irq_lock, flags);

	if ((iir & DE_PIPEA_EVENT) && (pipeastat & PIPE_VBLANK_INT_STATUS))
		drm_crtc_handle_vblank(&gdev->crtcs[PIPE_A].base);
	if ((iir & DE_PIPEB_EVENT) && (pipebstat & PIPE_VBLANK_INT_STATUS))
		drm_crtc_handle_vblank(&gdev->crtcs[PIPE_B].base);
	ret = IRQ_HANDLED;

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

	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_INTX);
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

	/* Vblank sources are enabled on demand; connectors use polling. */
	gdev->irq_mask = 0;
	gma3600_write(gdev, IMR, ~gdev->irq_mask);
	gma3600_write(gdev, IER, gdev->irq_mask);

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
