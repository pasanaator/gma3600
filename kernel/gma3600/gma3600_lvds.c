// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel GMA 3600 (Cedar Trail) DRM/KMS Driver — LVDS Flat Panel
 *
 * Copyright (C) 2026 Antigravity Project
 */

#include <linux/delay.h>
#include <linux/iopoll.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_probe_helper.h>

#include "gma3600_drv.h"
#include "gma3600_reg.h"

#define PANEL_POWER_TIMEOUT_US	300000

static void gma3600_lvds_set_power(struct gma3600_device *gdev, bool on)
{
	u32 val;

	val = gma3600_read(gdev, PP_CONTROL);
	if (on) {
		val |= POWER_TARGET_ON;
		gma3600_write_post(gdev, PP_CONTROL, val);
		read_poll_timeout_atomic(gma3600_read, val, (val & PP_ON),
					 1000, PANEL_POWER_TIMEOUT_US, false,
					 gdev, PP_STATUS);
	} else {
		val &= ~POWER_TARGET_ON;
		gma3600_write_post(gdev, PP_CONTROL, val);
		read_poll_timeout_atomic(gma3600_read, val, !(val & PP_ON),
					 1000, PANEL_POWER_TIMEOUT_US, false,
					 gdev, PP_STATUS);
	}
}

bool gma3600_lvds_detect_panel(struct gma3600_device *gdev)
{
	return gdev->has_lvds;
}

static enum drm_connector_status
gma3600_lvds_detect(struct drm_connector *connector, bool force)
{
	struct gma3600_connector *gconn = to_gma3600_connector(connector);
	struct gma3600_device *gdev = to_gma3600(connector->dev);
	struct edid *edid;

	if (!gdev->has_lvds)
		return connector_status_disconnected;

	if (gdev->lvds.fixed_mode)
		return connector_status_connected;

	if (gconn->ddc_bus) {
		edid = drm_get_edid(connector, &gconn->ddc_bus->adapter);
		if (edid) {
			kfree(edid);
			return connector_status_connected;
		}
	}

	return connector_status_disconnected;
}

static int gma3600_lvds_get_modes(struct drm_connector *connector)
{
	struct gma3600_connector *gconn = to_gma3600_connector(connector);
	struct gma3600_device *gdev = to_gma3600(connector->dev);
	struct edid *edid;
	int count = 0;

	if (gconn->ddc_bus) {
		edid = drm_get_edid(connector, &gconn->ddc_bus->adapter);
		if (edid) {
			drm_connector_update_edid_property(connector, edid);
			count = drm_add_edid_modes(connector, edid);
			kfree(edid);
		}
	}

	/* Fallback default netbook modes if EDID is absent */
	if (count == 0) {
		if (gdev->lvds.fixed_mode) {
			struct drm_display_mode *mode = drm_mode_duplicate(connector->dev,
									   gdev->lvds.fixed_mode);
			if (mode) {
				drm_mode_probed_add(connector, mode);
				count++;
			}
		} else {
			count += drm_add_modes_noedid(connector, 1366, 768);
			count += drm_add_modes_noedid(connector, 1024, 600);
		}
	}

	return count;
}

static enum drm_mode_status
gma3600_lvds_mode_valid(struct drm_connector *connector,
			struct drm_display_mode *mode)
{
	struct gma3600_device *gdev = to_gma3600(connector->dev);

	if (gdev->lvds.fixed_mode) {
		if (mode->hdisplay > gdev->lvds.fixed_mode->hdisplay ||
		    mode->vdisplay > gdev->lvds.fixed_mode->vdisplay)
			return MODE_PANEL;
	}

	if (mode->clock > 115000 && !gdev->lvds.is_dual_link)
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

static const struct drm_connector_funcs gma3600_lvds_connector_funcs = {
	.reset                  = drm_atomic_helper_connector_reset,
	.detect                 = gma3600_lvds_detect,
	.fill_modes             = drm_helper_probe_single_connector_modes,
	.destroy                = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state   = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_helper_funcs gma3600_lvds_conn_helper_funcs = {
	.get_modes  = gma3600_lvds_get_modes,
	.mode_valid = gma3600_lvds_mode_valid,
};

static void gma3600_lvds_encoder_atomic_enable(struct drm_encoder *encoder,
					       struct drm_atomic_state *state)
{
	struct gma3600_device *gdev = to_gma3600(encoder->dev);
	struct drm_connector *connector;
	struct drm_connector_state *conn_state;
	struct drm_crtc *crtc = NULL;
	enum gma3600_pipe pipe;
	u32 lvds;

	connector = drm_atomic_get_new_connector_for_encoder(state, encoder);
	if (connector) {
		conn_state = drm_atomic_get_new_connector_state(state, connector);
		if (conn_state)
			crtc = conn_state->crtc;
	}
	if (!crtc)
		crtc = encoder->crtc;
	if (!crtc)
		return;

	if (!to_gma3600_crtc(crtc)->active)
		return;

	pipe = to_gma3600_crtc(crtc)->pipe;

	/* Power sequence on */
	gma3600_lvds_set_power(gdev, true);

	lvds = LVDS_PORT_EN | LVDS_PIPE_SEL(pipe);
	lvds |= LVDS_A0A2_CLKA_POWER_UP;

	if (gdev->lvds.is_dual_link)
		lvds |= LVDS_B0B3_POWER_UP | LVDS_CLKB_POWER_UP;

	gma3600_write_post(gdev, LVDS, lvds);
	udelay(150);

	/* Unblank backlight */
	gma3600_backlight_set(gdev, gdev->backlight.level);
}

static void gma3600_lvds_encoder_atomic_disable(struct drm_encoder *encoder,
						struct drm_atomic_state *state)
{
	struct gma3600_device *gdev = to_gma3600(encoder->dev);
	u32 lvds;

	/* Blank backlight first to avoid white flash */
	gma3600_backlight_set(gdev, 0);

	lvds = gma3600_read(gdev, LVDS);
	lvds &= ~LVDS_PORT_EN;
	gma3600_write_post(gdev, LVDS, lvds);
	udelay(150);

	/* Power sequence off */
	gma3600_lvds_set_power(gdev, false);
}

static const struct drm_encoder_helper_funcs gma3600_lvds_enc_helper_funcs = {
	.atomic_enable  = gma3600_lvds_encoder_atomic_enable,
	.atomic_disable = gma3600_lvds_encoder_atomic_disable,
};

static const struct drm_encoder_funcs gma3600_lvds_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

int gma3600_lvds_init(struct gma3600_device *gdev)
{
	struct gma3600_encoder *genc;
	struct gma3600_connector *gconn;
	int ret;

	if (!gdev->has_lvds)
		return 0;

	genc = devm_kzalloc(gdev->drm.dev, sizeof(*genc), GFP_KERNEL);
	if (!genc)
		return -ENOMEM;

	gconn = devm_kzalloc(gdev->drm.dev, sizeof(*gconn), GFP_KERNEL);
	if (!gconn)
		return -ENOMEM;

	genc->gdev = gdev;
	genc->type = GMA3600_OUTPUT_LVDS;
	/* In Cedar Trail, LVDS is typically routed to Pipe B */
	genc->crtc_mask = (1 << PIPE_B) | (1 << PIPE_A);

	ret = drm_encoder_init(&gdev->drm, &genc->base, &gma3600_lvds_encoder_funcs,
			       DRM_MODE_ENCODER_LVDS, "LVDS-0");
	if (ret)
		return ret;

	drm_encoder_helper_add(&genc->base, &gma3600_lvds_enc_helper_funcs);
	genc->base.possible_crtcs = genc->crtc_mask;

	gconn->type = GMA3600_OUTPUT_LVDS;
	gconn->encoder = genc;
	gconn->ddc_bus = &gdev->gmbus[GMA3600_GMBUS_PANEL];

	ret = drm_connector_init(&gdev->drm, &gconn->base, &gma3600_lvds_connector_funcs,
				 DRM_MODE_CONNECTOR_LVDS);
	if (ret)
		goto err_cleanup_enc;

	drm_connector_helper_add(&gconn->base, &gma3600_lvds_conn_helper_funcs);

	ret = drm_connector_attach_encoder(&gconn->base, &genc->base);
	if (ret)
		goto err_cleanup_conn;

	return 0;

err_cleanup_conn:
	drm_connector_cleanup(&gconn->base);
err_cleanup_enc:
	drm_encoder_cleanup(&genc->base);
	return ret;
}
