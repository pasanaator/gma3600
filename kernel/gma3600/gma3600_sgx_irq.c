// SPDX-License-Identifier: GPL-2.0-only
/* SGX shares IER/IMR/IIR with display. Keep their ownership in the parent. */
#include <linux/interrupt.h>

#include "gma3600_drv.h"
#include "gma3600_reg.h"
#include "gma3600_sgx.h"

/* irq_lock held. Updating just SGX must preserve every enabled display bit. */
static void sgx_irq_mask(struct gma3600_device *gdev, bool enable)
{
	if (enable)
		gdev->irq_mask |= GMA3600_SGX_IRQ_BIT;
	else
		gdev->irq_mask &= ~GMA3600_SGX_IRQ_BIT;
	gma3600_write(gdev, IER, gdev->irq_mask);
	gma3600_write_post(gdev, IMR, ~gdev->irq_mask);
}

void gma3600_sgx_irq_init(struct gma3600_device *gdev)
{
	mutex_init(&gdev->sgx_irq_mutex);
	gdev->sgx_irq_closing = false;
	gdev->sgx_irq_handler = NULL;
	gdev->sgx_irq_data = NULL;
}

static int sgx_irq_register(struct gma3600_sgx_device *sgx,
			    irqreturn_t (*handler)(void *), void *data)
{
	struct gma3600_device *gdev = sgx->host;
	unsigned long flags;
	int ret = 0;

	if (!handler || !data)
		return -EINVAL;
	mutex_lock(&gdev->sgx_irq_mutex);
	spin_lock_irqsave(&gdev->irq_lock, flags);
	if (gdev->sgx_irq_closing)
		ret = -ENODEV;
	else if (gdev->sgx_irq_handler)
		ret = -EBUSY;
	else {
		gdev->sgx_irq_handler = handler;
		gdev->sgx_irq_data = data;
	}
	spin_unlock_irqrestore(&gdev->irq_lock, flags);
	mutex_unlock(&gdev->sgx_irq_mutex);
	return ret;
}

static int sgx_irq_enable(struct gma3600_sgx_device *sgx, void *data, bool enable)
{
	struct gma3600_device *gdev = sgx->host;
	unsigned long flags;
	int ret = 0;

	mutex_lock(&gdev->sgx_irq_mutex);
	spin_lock_irqsave(&gdev->irq_lock, flags);
	if (gdev->sgx_irq_closing)
		ret = -ENODEV;
	else if (!gdev->sgx_irq_handler || gdev->sgx_irq_data != data)
		ret = -ENOENT;
	else
		sgx_irq_mask(gdev, enable);
	spin_unlock_irqrestore(&gdev->irq_lock, flags);
	mutex_unlock(&gdev->sgx_irq_mutex);
	return ret;
}

/* sgx_irq_mutex held; prevent replacement until any copied ISR has returned. */
static void sgx_irq_detach(struct gma3600_device *gdev)
{
	unsigned long flags;

	spin_lock_irqsave(&gdev->irq_lock, flags);
	sgx_irq_mask(gdev, false);
	gdev->sgx_irq_handler = NULL;
	gdev->sgx_irq_data = NULL;
	spin_unlock_irqrestore(&gdev->irq_lock, flags);
	synchronize_irq(gdev->pdev->irq);
}

static void sgx_irq_unregister(struct gma3600_sgx_device *sgx, void *data)
{
	struct gma3600_device *gdev = sgx->host;

	mutex_lock(&gdev->sgx_irq_mutex);
	if (gdev->sgx_irq_handler && gdev->sgx_irq_data == data)
		sgx_irq_detach(gdev);
	mutex_unlock(&gdev->sgx_irq_mutex);
}

void gma3600_sgx_irq_shutdown(struct gma3600_device *gdev)
{
	mutex_lock(&gdev->sgx_irq_mutex);
	gdev->sgx_irq_closing = true;
	sgx_irq_detach(gdev);
	mutex_unlock(&gdev->sgx_irq_mutex);
}

void gma3600_sgx_irq_dispatch(struct gma3600_device *gdev, u32 iir)
{
	irqreturn_t (*handler)(void *);
	void *data;
	unsigned long flags;

	if (!(iir & GMA3600_SGX_IRQ_BIT))
		return;
	spin_lock_irqsave(&gdev->irq_lock, flags);
	handler = gdev->sgx_irq_handler;
	data = gdev->sgx_irq_data;
	spin_unlock_irqrestore(&gdev->irq_lock, flags);
	if (handler && handler(data) == IRQ_HANDLED)
		return;

	/* A missing/unhandled source must not leave the shared line storming. */
	spin_lock_irqsave(&gdev->irq_lock, flags);
	sgx_irq_mask(gdev, false);
	spin_unlock_irqrestore(&gdev->irq_lock, flags);
	dev_warn_ratelimited(&gdev->pdev->dev, "Masked unhandled SGX interrupt\n");
}

const struct gma3600_sgx_irq_ops gma3600_sgx_irq_ops = {
	.register_handler = sgx_irq_register,
	.enable = sgx_irq_enable,
	.unregister_handler = sgx_irq_unregister,
};
