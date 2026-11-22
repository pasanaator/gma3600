// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel GMA 3600 (Cedar Trail) DRM/KMS Driver — Backlight Subsystem
 *
 * Copyright (C) 2026 Antigravity Project
 */

#include <linux/backlight.h>
#include <linux/pci.h>

#include "gma3600_drv.h"
#include "gma3600_reg.h"

#define GMA3600_DEFAULT_BACKLIGHT_FREQ	0x07F0	/* Typical PWM max frequency */

static int gma3600_bl_get_brightness(struct backlight_device *bd)
{
	struct gma3600_device *gdev = bl_get_data(bd);
	u32 val;

	val = gma3600_read(gdev, BLC_PWM_CTL);
	return val & BL_DUTY_MASK;
}

static int gma3600_bl_update_status(struct backlight_device *bd)
{
	struct gma3600_device *gdev = bl_get_data(bd);
	u32 level = bd->props.brightness;
	u32 val, max;

	if (backlight_is_blank(bd))
		level = 0;

	val = gma3600_read(gdev, BLC_PWM_CTL);
	max = (val >> BL_MAX_SHIFT) & 0xFFFF;
	if (max == 0) {
		max = GMA3600_DEFAULT_BACKLIGHT_FREQ;
		val = max << BL_MAX_SHIFT;
	}

	if (level > max)
		level = max;

	val = (val & ~BL_DUTY_MASK) | (level & BL_DUTY_MASK);
	gma3600_write(gdev, BLC_PWM_CTL, val);

	/* Ensure PWM generator is enabled */
	val = gma3600_read(gdev, BLC_PWM_CTL2);
	if (level > 0)
		val |= BLM_PWM_ENABLE;
	else
		val &= ~BLM_PWM_ENABLE;
	gma3600_write(gdev, BLC_PWM_CTL2, val);

	gdev->backlight.level = level;
	return 0;
}

static const struct backlight_ops gma3600_bl_ops = {
	.get_brightness = gma3600_bl_get_brightness,
	.update_status  = gma3600_bl_update_status,
};

void gma3600_backlight_set(struct gma3600_device *gdev, u32 level)
{
	if (!gdev->backlight.device)
		return;

	gdev->backlight.device->props.brightness = level;
	backlight_update_status(gdev->backlight.device);
}

u32 gma3600_backlight_get(struct gma3600_device *gdev)
{
	if (!gdev->backlight.device)
		return 0;

	return gdev->backlight.device->props.brightness;
}

int gma3600_backlight_init(struct gma3600_device *gdev)
{
	struct backlight_properties props;
	struct backlight_device *bd;
	u32 ctl, max;

	if (!gdev->has_lvds)
		return 0;

	ctl = gma3600_read(gdev, BLC_PWM_CTL);
	max = (ctl >> BL_MAX_SHIFT) & 0xFFFF;
	if (max == 0) {
		max = GMA3600_DEFAULT_BACKLIGHT_FREQ;
		ctl = (max << BL_MAX_SHIFT) | (max / 2);
		gma3600_write(gdev, BLC_PWM_CTL, ctl);
	}

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = max;
	props.brightness = ctl & BL_DUTY_MASK;
	if (props.brightness == 0)
		props.brightness = max / 2;

	bd = backlight_device_register("gma3600_backlight",
				       &gdev->pdev->dev,
				       gdev,
				       &gma3600_bl_ops,
				       &props);
	if (IS_ERR(bd)) {
		dev_err(&gdev->pdev->dev, "Failed to register backlight device\n");
		return PTR_ERR(bd);
	}

	gdev->backlight.device = bd;
	gdev->backlight.max_brightness = max;
	gdev->backlight.level = props.brightness;
	gdev->backlight.enabled = true;

	/* Enable PWM control in hardware */
	gma3600_write(gdev, BLC_PWM_CTL2,
		      gma3600_read(gdev, BLC_PWM_CTL2) | BLM_PWM_ENABLE);

	return 0;
}

void gma3600_backlight_fini(struct gma3600_device *gdev)
{
	if (gdev->backlight.device) {
		backlight_device_unregister(gdev->backlight.device);
		gdev->backlight.device = NULL;
	}
}
