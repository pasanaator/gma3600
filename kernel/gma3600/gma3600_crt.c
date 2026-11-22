// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel GMA 3600 (Cedar Trail) DRM/KMS Driver — Analog CRT / VGA
 *
 * Copyright (C) 2026 Antigravity Project
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_probe_helper.h>
#include "gma3600_drv.h"
#include "gma3600_reg.h"

static enum drm_connector_status
gma3600_crt_detect(struct drm_connector *connector, bool force)
{
	struct gma3600_connector *gconn = to_gma3600_connector(connector);
	struct edid *edid;
	enum drm_connector_status status = connector_status_disconnected;

	if (gconn->ddc_bus) {
		edid = drm_get_edid(connector, &gconn->ddc_bus->adapter);
		if (edid) {
			kfree(edid);
			return connector_status_connected;
		}
	}

	return status;
}

static int gma3600_crt_get_modes(struct drm_connector *connector)
{
	struct gma3600_connector *gconn = to_gma3600_connector(connector);
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

	if (count == 0)
		count = drm_add_modes_noedid(connector, 1920, 1080);

	return count;
}

static enum drm_mode_status
gma3600_crt_mode_valid(struct drm_connector *connector,
		       struct drm_display_mode *mode)
{
	if (mode->clock > 355000)
		return MODE_CLOCK_HIGH;
	if (mode->clock < 20000)
		return MODE_CLOCK_LOW;

	return MODE_OK;
}

static const struct drm_connector_funcs gma3600_crt_connector_funcs = {
	.reset                  = drm_atomic_helper_connector_reset,
	.detect                 = gma3600_crt_detect,
	.fill_modes             = drm_helper_probe_single_connector_modes,
	.destroy                = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state   = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_helper_funcs gma3600_crt_conn_helper_funcs = {
	.get_modes  = gma3600_crt_get_modes,
	.mode_valid = gma3600_crt_mode_valid,
};

static void gma3600_crt_encoder_atomic_enable(struct drm_encoder *encoder,
					      struct drm_atomic_state *state)
{
	struct gma3600_device *gdev = to_gma3600(encoder->dev);
	struct drm_connector *connector;
	struct drm_connector_state *conn_state;
	struct drm_crtc *crtc = NULL;
	struct drm_crtc_state *crtc_state;
	struct drm_display_mode *mode;
	enum gma3600_pipe pipe;
	u32 adpa = ADPA_DAC_ENABLE;

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

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	if (!crtc_state)
		crtc_state = crtc->state;
	if (!crtc_state)
		return;

	if (!to_gma3600_crtc(crtc)->active)
		return;

	pipe = to_gma3600_crtc(crtc)->pipe;
	mode = &crtc_state->adjusted_mode;

	if (pipe == PIPE_B)
		adpa |= ADPA_PIPE_B_SELECT;
	else
		adpa |= ADPA_PIPE_A_SELECT;

	if (mode->flags & DRM_MODE_FLAG_PHSYNC)
		adpa |= ADPA_HSYNC_ACTIVE_HIGH;
	if (mode->flags & DRM_MODE_FLAG_PVSYNC)
		adpa |= ADPA_VSYNC_ACTIVE_HIGH;

	gma3600_write_post(gdev, ADPA, adpa);
}

static void gma3600_crt_encoder_atomic_disable(struct drm_encoder *encoder,
					       struct drm_atomic_state *state)
{
	struct gma3600_device *gdev = to_gma3600(encoder->dev);

	gma3600_write_post(gdev, ADPA, ADPA_VSYNC_CNTL_DISABLE | ADPA_HSYNC_CNTL_DISABLE);
}

static const struct drm_encoder_helper_funcs gma3600_crt_enc_helper_funcs = {
	.atomic_enable  = gma3600_crt_encoder_atomic_enable,
	.atomic_disable = gma3600_crt_encoder_atomic_disable,
};

static const struct drm_encoder_funcs gma3600_crt_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

int gma3600_crt_init(struct gma3600_device *gdev)
{
	struct gma3600_encoder *genc;
	struct gma3600_connector *gconn;
	int ret;

	genc = devm_kzalloc(gdev->drm.dev, sizeof(*genc), GFP_KERNEL);
	if (!genc)
		return -ENOMEM;

	gconn = devm_kzalloc(gdev->drm.dev, sizeof(*gconn), GFP_KERNEL);
	if (!gconn)
		return -ENOMEM;

	genc->gdev = gdev;
	genc->type = GMA3600_OUTPUT_ANALOG;
	genc->crtc_mask = (1 << PIPE_A);

	ret = drm_encoder_init(&gdev->drm, &genc->base, &gma3600_crt_encoder_funcs,
			       DRM_MODE_ENCODER_DAC, "CRT-0");
	if (ret)
		return ret;

	drm_encoder_helper_add(&genc->base, &gma3600_crt_enc_helper_funcs);
	genc->base.possible_crtcs = genc->crtc_mask;

	gconn->type = GMA3600_OUTPUT_ANALOG;
	gconn->encoder = genc;
	gconn->ddc_bus = &gdev->gmbus[GMA3600_GMBUS_VGADDC];

	ret = drm_connector_init(&gdev->drm, &gconn->base, &gma3600_crt_connector_funcs,
				 DRM_MODE_CONNECTOR_VGA);
	if (ret)
		goto err_cleanup_enc;

	gconn->base.polled = DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT;
	drm_connector_helper_add(&gconn->base, &gma3600_crt_conn_helper_funcs);
	gconn->base.interlace_allowed = 0;
	gconn->base.doublescan_allowed = 0;

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
