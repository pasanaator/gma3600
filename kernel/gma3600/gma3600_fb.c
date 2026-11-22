// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel GMA 3600 (Cedar Trail) DRM/KMS Driver — Framebuffer & FBDEV Emulation
 *
 * Copyright (C) 2026 Antigravity Project
 */

#include <linux/version.h>

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_framebuffer_helper.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#include <drm/drm_client_setup.h>
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
#include <drm/drm_fbdev_ttm.h>
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
#include <drm/drm_fbdev_generic.h>
#else
#include <drm/drm_fb_helper.h>
#endif

#include "gma3600_drv.h"

static const struct drm_framebuffer_funcs gma3600_fb_funcs = {
	.destroy       = drm_gem_fb_destroy,
	.create_handle = drm_gem_fb_create_handle,
};

struct drm_framebuffer *
gma3600_fb_create(struct drm_device *dev, struct drm_file *file,
		  const struct drm_mode_fb_cmd2 *cmd)
{
	const struct drm_format_info *info;

	info = drm_get_format_info(dev, cmd);
	if (!info || (cmd->pitches[0] & 63))
		return ERR_PTR(-EINVAL);

	switch (cmd->pixel_format) {
	case DRM_FORMAT_RGB565:
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		break;
	default:
		dev_err(dev->dev, "Unsupported pixel format 0x%08x\n",
			cmd->pixel_format);
		return ERR_PTR(-EINVAL);
	}

	return drm_gem_fb_create_with_funcs(dev, file, cmd, &gma3600_fb_funcs);
}

int gma3600_fbdev_init(struct gma3600_device *gdev)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
	drm_client_setup_with_color_mode(&gdev->drm, 32);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
	drm_fbdev_ttm_setup(&gdev->drm, 32);
#else
	drm_fbdev_generic_setup(&gdev->drm, 32);
#endif
	return 0;
}
