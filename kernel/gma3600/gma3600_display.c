// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel GMA 3600 (Cedar Trail) DRM/KMS Driver — Atomic KMS Display Subsystem
 *
 * Copyright (C) 2026 Antigravity Project
 */

#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/version.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include "gma3600_drv.h"
#include "gma3600_reg.h"

/* Supported pixel formats for primary planes */
static const u32 gma3600_primary_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_ABGR8888,
};

/* Supported pixel formats for hardware cursor planes */
static const u32 gma3600_cursor_formats[] = {
	DRM_FORMAT_ARGB8888,
};

/* ================================================================
 * Primary Plane Implementation
 * ================================================================ */

static int gma3600_plane_atomic_check(struct drm_plane *plane,
				      struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state;

	if (!new_plane_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_crtc_state(state,
						   new_plane_state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	return drm_atomic_helper_check_plane_state(new_plane_state,
						   crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   false, true);
}

static void gma3600_plane_atomic_update(struct drm_plane *plane,
					struct drm_atomic_state *state)
{
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_framebuffer *fb = new_state->fb;
	struct gma3600_device *gdev = to_gma3600(plane->dev);
	struct gma3600_crtc *gcrtc;
	struct gma3600_gem_object *obj;
	enum gma3600_pipe pipe;
	u32 dspcntr = DSP_ENABLE;
	u32 start_addr;

	if (!fb || !new_state->crtc)
		return;

	gcrtc = to_gma3600_crtc(new_state->crtc);
	pipe = gcrtc->pipe;
	obj = to_gma3600_gem(drm_gem_fb_get_obj(fb, 0));

	switch (fb->format->format) {
	case DRM_FORMAT_RGB565:
		dspcntr |= DSP_FORMAT_RGB565;
		break;
	case DRM_FORMAT_XRGB8888:
		dspcntr |= DSP_FORMAT_XRGB8888;
		break;
	case DRM_FORMAT_ARGB8888:
		dspcntr |= DSP_FORMAT_ARGB8888;
		break;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		dspcntr |= DSP_FORMAT_XBGR8888;
		break;
	}

	if (pipe == PIPE_B)
		dspcntr |= DSP_PIPE_SEL_B;
	else
		dspcntr |= DSP_PIPE_SEL_A;

	/* Use direct color until a gamma LUT has been installed. */

	/* Calculate GTT byte address */
	start_addr = obj->gtt_offset + fb->offsets[0] +
		     (new_state->src.y1 >> 16) * fb->pitches[0] +
		     (new_state->src.x1 >> 16) * fb->format->cpp[0];

	gma3600_write(gdev, DSPSTRIDE(pipe), fb->pitches[0]);
	gma3600_write(gdev, DSPLINOFF(pipe), start_addr & ~PAGE_MASK);
	gma3600_write(gdev, DSPPOS(pipe), 0);
	gma3600_write(gdev, DSPSIZE(pipe),
		      (((new_state->crtc_h - 1) & 0xFFF) << 16) |
		      ((new_state->crtc_w - 1) & 0xFFF));
	gma3600_write(gdev, DSPCNTR(pipe), dspcntr);

	/* Writing DSPSURF latches all plane registers on next VBlank */
	gma3600_write_post(gdev, DSPSURF(pipe), start_addr & PAGE_MASK);
}

static void gma3600_plane_atomic_disable(struct drm_plane *plane,
					 struct drm_atomic_state *state)
{
	struct drm_plane_state *old_state =
		drm_atomic_get_old_plane_state(state, plane);
	struct gma3600_device *gdev = to_gma3600(plane->dev);
	enum gma3600_pipe pipe;

	if (!old_state->crtc)
		return;

	pipe = to_gma3600_crtc(old_state->crtc)->pipe;
	gma3600_write(gdev, DSPCNTR(pipe), 0);
	gma3600_write_post(gdev, DSPSURF(pipe), 0);
}

static const struct drm_plane_helper_funcs gma3600_primary_plane_helper = {
	.atomic_check   = gma3600_plane_atomic_check,
	.atomic_update  = gma3600_plane_atomic_update,
	.atomic_disable = gma3600_plane_atomic_disable,
};

static const struct drm_plane_funcs gma3600_primary_plane_funcs = {
	.update_plane           = drm_atomic_helper_update_plane,
	.disable_plane          = drm_atomic_helper_disable_plane,
	.destroy                = drm_plane_cleanup,
	.reset                  = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state   = drm_atomic_helper_plane_destroy_state,
};

/* ================================================================
 * Hardware Cursor Plane Implementation
 * ================================================================ */

static int gma3600_cursor_atomic_check(struct drm_plane *plane,
				       struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state =
		drm_atomic_get_new_plane_state(state, plane);

	if (!new_plane_state->fb)
		return 0;

	if (new_plane_state->crtc_w > GMA3600_CURSOR_W ||
	    new_plane_state->crtc_h > GMA3600_CURSOR_H)
		return -EINVAL;

	return 0;
}

static void gma3600_cursor_atomic_update(struct drm_plane *plane,
					 struct drm_atomic_state *state)
{
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_framebuffer *fb = new_state->fb;
	struct gma3600_device *gdev = to_gma3600(plane->dev);
	struct gma3600_crtc *gcrtc;
	struct gma3600_gem_object *obj;
	enum gma3600_pipe pipe;
	u32 pos = 0;
	int x, y;

	if (!fb || !new_state->crtc) {
		/* Shouldn't reach here, but handle defensively */
		return;
	}

	gcrtc = to_gma3600_crtc(new_state->crtc);
	pipe = gcrtc->pipe;
	obj = to_gma3600_gem(drm_gem_fb_get_obj(fb, 0));

	x = new_state->crtc_x;
	y = new_state->crtc_y;

	if (x < 0) {
		pos |= CURSOR_X_SIGN;
		x = -x;
	}
	pos |= (x & 0x1FFF);

	if (y < 0) {
		pos |= CURSOR_Y_SIGN;
		y = -y;
	}
	pos |= ((y & 0x1FFF) << 16);

	gma3600_write(gdev, CURPOS(pipe), pos);
	gma3600_write(gdev, CURCNTR(pipe),
		      CURSOR_MODE_64_ARGB_AX);

	/* Writing CURBASE latches cursor updates */
	gma3600_write_post(gdev, CURBASE(pipe), obj->gtt_offset);
}

static void gma3600_cursor_atomic_disable(struct drm_plane *plane,
					  struct drm_atomic_state *state)
{
	struct drm_plane_state *old_state =
		drm_atomic_get_old_plane_state(state, plane);
	struct gma3600_device *gdev = to_gma3600(plane->dev);
	enum gma3600_pipe pipe;

	if (!old_state->crtc)
		return;

	pipe = to_gma3600_crtc(old_state->crtc)->pipe;
	gma3600_write(gdev, CURCNTR(pipe), CURSOR_MODE_DISABLE);
	gma3600_write_post(gdev, CURBASE(pipe), 0);
}

static const struct drm_plane_helper_funcs gma3600_cursor_plane_helper = {
	.atomic_check   = gma3600_cursor_atomic_check,
	.atomic_update  = gma3600_cursor_atomic_update,
	.atomic_disable = gma3600_cursor_atomic_disable,
};

static const struct drm_plane_funcs gma3600_cursor_plane_funcs = {
	.update_plane           = drm_atomic_helper_update_plane,
	.disable_plane          = drm_atomic_helper_disable_plane,
	.destroy                = drm_plane_cleanup,
	.reset                  = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state   = drm_atomic_helper_plane_destroy_state,
};

/* ================================================================
 * FIFO Watermark Configuration
 * ================================================================ */

void gma3600_update_wm(struct gma3600_device *gdev, enum gma3600_pipe pipe, bool is_lvds)
{
	u32 fw;

	fw = gma3600_read(gdev, DSPFW1);
	fw &= ~DSP_FIFO_SR_WM_MASK;
	fw |= (0x7EU << DSP_FIFO_SR_WM_SHIFT);	/* SR WM */
	fw &= ~CURSOR_B_FIFO_WM_MASK;
	fw |= (0x04U << CURSOR_B_FIFO_WM_SHIFT);	/* Cursor B WM */
	gma3600_write(gdev, DSPFW1, fw);

	fw = gma3600_read(gdev, DSPFW2);
	fw &= ~0x00003F00;
	fw |= (0x06 << 8);	/* Cursor A WM */
	fw &= ~0x0000007F;
	fw |= 0x08;		/* Plane C WM */
	gma3600_write(gdev, DSPFW2, fw);

	gma3600_write(gdev, DSPFW3, 0x36000000);

	if (pipe == PIPE_B && is_lvds) {
		gma3600_write(gdev, DSPFW5, 0x00040330);
	} else {
		fw = (3 << 24) | (4 << 16) | (3 << 8) | 4;
		gma3600_write(gdev, DSPFW5, fw);
	}

	gma3600_write(gdev, DSPFW6, 0x10);

	/*
	 * Self-refresh (CxSR) causes memory clock gating which starves the display FIFO
	 * on desktop Atom SKUs, causing video output sync failure. Only enable on LVDS netbooks.
	 */
	if (is_lvds) {
		gma3600_write(gdev, FW_BLC_SELF, FW_BLC_SELF_EN);
		gma3600_read(gdev, FW_BLC_SELF);
	} else {
		gma3600_write(gdev, FW_BLC_SELF, 0);
		gma3600_read(gdev, FW_BLC_SELF);
	}
}

/* ================================================================
 * CRTC Implementation
 * ================================================================ */

/* Use the encoder mask in the transaction, never a connector's old state. */
static int gma3600_crtc_clock(struct drm_crtc *crtc,
			      const struct drm_crtc_state *state,
			      struct gma3600_dpll_clock *clock,
			      bool *is_lvds, bool *is_hdmi)
{
	struct gma3600_device *gdev = to_gma3600(crtc->dev);
	struct drm_encoder *encoder;
	const struct gma3600_dpll_limits *limits;
	int refclk = gdev->dplla_96mhz ? 96000 : 27000;

	*is_lvds = false;
	*is_hdmi = false;
	drm_for_each_encoder_mask(encoder, crtc->dev, state->encoder_mask) {
		*is_lvds |= to_gma3600_encoder(encoder)->type == GMA3600_OUTPUT_LVDS;
		*is_hdmi |= to_gma3600_encoder(encoder)->type == GMA3600_OUTPUT_HDMI;
	}
	if (*is_lvds && gdev->lvds_use_ssc)
		refclk = gdev->lvds_ssc_freq;
	limits = gma3600_dpll_get_limits(*is_lvds ? GMA3600_OUTPUT_LVDS :
					GMA3600_OUTPUT_ANALOG, refclk);
	return gma3600_dpll_find_best(refclk, state->adjusted_mode.clock,
				     limits, clock) ? 0 : -EINVAL;
}

static int gma3600_crtc_atomic_check(struct drm_crtc *crtc,
				     struct drm_atomic_state *state)
{
	struct drm_crtc_state *new = drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_display_mode *m = &new->adjusted_mode;
	struct gma3600_dpll_clock clock;
	bool is_lvds, is_hdmi;

	if (!new->enable)
		return 0;
	/* No interlaced/doublescan programming or scaling is implemented. */
	if (m->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN) ||
	    m->hdisplay <= 0 || m->vdisplay <= 0 ||
	    m->hdisplay > m->hsync_start || m->hsync_start >= m->hsync_end ||
	    m->hsync_end > m->htotal || m->vdisplay > m->vsync_start ||
	    m->vsync_start >= m->vsync_end || m->vsync_end > m->vtotal ||
	    m->htotal > 8192 || m->vtotal > 8192)
		return -EINVAL;
	drm_mode_set_crtcinfo(m, 0);
	return gma3600_crtc_clock(crtc, new, &clock, &is_lvds, &is_hdmi);
}

static void gma3600_crtc_atomic_enable(struct drm_crtc *crtc,
				       struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_display_mode *mode = &crtc_state->adjusted_mode;
	struct gma3600_device *gdev = to_gma3600(crtc->dev);
	struct gma3600_crtc *gcrtc = to_gma3600_crtc(crtc);
	enum gma3600_pipe pipe = gcrtc->pipe;
	struct gma3600_dpll_clock clock;
	u32 pipeconf;
	bool is_lvds, is_hdmi;
	int ret;

	gcrtc->active = false;
	ret = gma3600_crtc_clock(crtc, crtc_state, &clock, &is_lvds, &is_hdmi);
	if (ret)
		return; /* Already rejected during atomic_check. */

	/* LVDS clock pairs must be powered before enabling the VCO. */
	if (is_lvds)
		gma3600_write_post(gdev, LVDS, gma3600_read(gdev, LVDS) |
				    LVDS_PORT_EN | LVDS_A0A2_CLKA_POWER_UP |
				    LVDS_PIPE_B_SELECT);

	dev_info(gdev->drm.dev,
		 "Pipe %c: %dx%d clock %d -> %d kHz (N=%d M2=%d P1=%d P2=%d)\n",
		 'A' + pipe, mode->hdisplay, mode->vdisplay, mode->clock,
		 clock.dot, clock.n, clock.m2, clock.p1, clock.p2);
	ret = gma3600_dpll_program(gdev, pipe, &clock, is_lvds, is_hdmi);
	if (ret) {
		gma3600_dump_display(gdev, "PLL failure");
		dev_err(gdev->drm.dev, "Pipe %c: PLL failed (%d); keeping output disabled\n",
			'A' + pipe, ret);
		gma3600_dpll_disable(gdev, pipe);
		if (is_lvds)
			gma3600_write_post(gdev, LVDS,
				gma3600_read(gdev, LVDS) & ~LVDS_PORT_EN);
		return;
	}

	/* Program FIFO watermarks to prevent display underrun */
	gma3600_update_wm(gdev, pipe, is_lvds);

	/*
	 * Program Pipe Timing Registers.
	 * Hardware format per Intel Cedarview specification:
	 *   [31:16] = total / blank_end / sync_end - 1
	 *   [15:0]  = active / blank_start / sync_start - 1
	 */
	gma3600_write(gdev, HTOTAL(pipe),
		      ((u32)(mode->crtc_hdisplay - 1) & 0xFFFF) |
		      (((u32)(mode->crtc_htotal - 1) & 0xFFFF) << 16));

	gma3600_write(gdev, HBLANK(pipe),
		      ((u32)(mode->crtc_hblank_start - 1) & 0xFFFF) |
		      (((u32)(mode->crtc_hblank_end - 1) & 0xFFFF) << 16));

	gma3600_write(gdev, HSYNC(pipe),
		      ((u32)(mode->crtc_hsync_start - 1) & 0xFFFF) |
		      (((u32)(mode->crtc_hsync_end - 1) & 0xFFFF) << 16));

	gma3600_write(gdev, VTOTAL(pipe),
		      ((u32)(mode->crtc_vdisplay - 1) & 0xFFFF) |
		      (((u32)(mode->crtc_vtotal - 1) & 0xFFFF) << 16));

	gma3600_write(gdev, VBLANK(pipe),
		      ((u32)(mode->crtc_vblank_start - 1) & 0xFFFF) |
		      (((u32)(mode->crtc_vblank_end - 1) & 0xFFFF) << 16));

	gma3600_write(gdev, VSYNC(pipe),
		      ((u32)(mode->crtc_vsync_start - 1) & 0xFFFF) |
		      (((u32)(mode->crtc_vsync_end - 1) & 0xFFFF) << 16));

	/* Program Pipe Source Image Size: [28:16] = H_size - 1, [11:0] = V_size - 1 */
	gma3600_write(gdev, PIPESRC(pipe),
		      (((u32)(mode->crtc_hdisplay - 1) & 0xFFF) << 16) |
		      ((u32)(mode->crtc_vdisplay - 1) & 0xFFF));

	/* Enable CRTC Display Pipe */
	pipeconf = PIPE_ENABLE | PIPE_8BPC;
	gma3600_write_post(gdev, PIPECONF(pipe), pipeconf);
	ret = read_poll_timeout(gma3600_read, pipeconf,
				pipeconf & PIPE_STATE_ENABLED,
				100, 100000, false, gdev, PIPECONF(pipe));
	if (ret) {
		gma3600_dump_display(gdev, "pipe enable failure");
		dev_err(gdev->drm.dev, "Pipe %c did not enable\n", 'A' + pipe);
		gma3600_write_post(gdev, PIPECONF(pipe), 0);
		gma3600_dpll_disable(gdev, pipe);
		return;
	}

	drm_crtc_vblank_on(crtc);
	gcrtc->active = true;
}

static void gma3600_crtc_atomic_disable(struct drm_crtc *crtc,
					struct drm_atomic_state *state)
{
	struct gma3600_device *gdev = to_gma3600(crtc->dev);
	struct gma3600_crtc *gcrtc = to_gma3600_crtc(crtc);
	enum gma3600_pipe pipe = gcrtc->pipe;

	gcrtc->active = false;
	drm_crtc_vblank_off(crtc);

	/* Turn off pipe */
	gma3600_write_post(gdev, PIPECONF(pipe), 0);
	udelay(150);

	/* Shut down DPLL */
	gma3600_dpll_disable(gdev, pipe);
}

static void gma3600_crtc_atomic_flush(struct drm_crtc *crtc,
				      struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);
	struct gma3600_device *gdev = to_gma3600(crtc->dev);
	unsigned long flags;

	if (crtc_state->event) {
		spin_lock_irqsave(&gdev->drm.event_lock, flags);

		if (drm_crtc_vblank_get(crtc) == 0)
			drm_crtc_arm_vblank_event(crtc, crtc_state->event);
		else
			drm_crtc_send_vblank_event(crtc, crtc_state->event);

		spin_unlock_irqrestore(&gdev->drm.event_lock, flags);
		crtc_state->event = NULL;
	}
}

static const struct drm_crtc_helper_funcs gma3600_crtc_helper_funcs = {
	.atomic_check   = gma3600_crtc_atomic_check,
	.atomic_enable  = gma3600_crtc_atomic_enable,
	.atomic_disable = gma3600_crtc_atomic_disable,
	.atomic_flush   = gma3600_crtc_atomic_flush,
};

/* VBlank enable/disable — declared before the CRTC funcs table that
 * references them.
 */
static int gma3600_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct gma3600_device *gdev = to_gma3600(crtc->dev);
	struct gma3600_crtc *gcrtc = to_gma3600_crtc(crtc);

	gma3600_irq_enable_vblank(gdev, gcrtc->pipe);
	return 0;
}

static void gma3600_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct gma3600_device *gdev = to_gma3600(crtc->dev);
	struct gma3600_crtc *gcrtc = to_gma3600_crtc(crtc);

	gma3600_irq_disable_vblank(gdev, gcrtc->pipe);
}

static const struct drm_crtc_funcs gma3600_crtc_funcs = {
	.reset                  = drm_atomic_helper_crtc_reset,
	.destroy                = drm_crtc_cleanup,
	.set_config             = drm_atomic_helper_set_config,
	.page_flip              = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state   = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank          = gma3600_crtc_enable_vblank,
	.disable_vblank         = gma3600_crtc_disable_vblank,
};

/* ================================================================
 * Mode Config Setup
 * ================================================================ */

static const struct drm_mode_config_funcs gma3600_mode_config_funcs = {
	.fb_create     = gma3600_fb_create,
	.atomic_check  = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

int gma3600_display_init(struct gma3600_device *gdev)
{
	struct drm_device *dev = &gdev->drm;
	enum gma3600_pipe pipe;
	int ret;

	ret = drmm_mode_config_init(dev);
	if (ret)
		return ret;

	dev->mode_config.min_width  = 320;
	dev->mode_config.min_height = 200;
	dev->mode_config.max_width  = 2048;
	dev->mode_config.max_height = 2048;
	dev->mode_config.funcs      = &gma3600_mode_config_funcs;

	/* Initialize VBlank for both Pipe A and Pipe B */
	ret = drm_vblank_init(dev, GMA3600_MAX_PIPES);
	if (ret) {
		dev_err(dev->dev, "drm_vblank_init failed: %d\n", ret);
		return ret;
	}

	for (pipe = 0; pipe < GMA3600_MAX_PIPES; pipe++) {
		struct gma3600_crtc *gcrtc = &gdev->crtcs[pipe];

		gcrtc->pipe = pipe;

		/* Primary plane */
		ret = drm_universal_plane_init(dev, &gcrtc->primary,
					       1 << pipe,
					       &gma3600_primary_plane_funcs,
					       gma3600_primary_formats,
					       ARRAY_SIZE(gma3600_primary_formats),
					       NULL,
					       DRM_PLANE_TYPE_PRIMARY,
					       "primary-%c", 'A' + pipe);
		if (ret)
			return ret;

		drm_plane_helper_add(&gcrtc->primary,
				     &gma3600_primary_plane_helper);

		/* Hardware cursor plane */
		ret = drm_universal_plane_init(dev, &gcrtc->cursor,
					       1 << pipe,
					       &gma3600_cursor_plane_funcs,
					       gma3600_cursor_formats,
					       ARRAY_SIZE(gma3600_cursor_formats),
					       NULL,
					       DRM_PLANE_TYPE_CURSOR,
					       "cursor-%c", 'A' + pipe);
		if (ret)
			return ret;

		drm_plane_helper_add(&gcrtc->cursor,
				     &gma3600_cursor_plane_helper);

		/* CRTC — static const funcs with vblank callbacks */
		ret = drm_crtc_init_with_planes(dev, &gcrtc->base,
						&gcrtc->primary,
						&gcrtc->cursor,
						&gma3600_crtc_funcs,
						"pipe-%c", 'A' + pipe);
		if (ret)
			return ret;

		drm_crtc_helper_add(&gcrtc->base, &gma3600_crtc_helper_funcs);
	}

	/* Initialize CRT (VGA) Output */
	ret = gma3600_crt_init(gdev);
	if (ret)
		dev_warn(dev->dev, "CRT output init failed: %d\n", ret);

	/* Initialize LVDS (Flat Panel) Output if present */
	if (gdev->has_lvds) {
		ret = gma3600_lvds_init(gdev);
		if (ret)
			dev_warn(dev->dev, "LVDS output init failed: %d\n",
				 ret);
	}

	/* Initialize HDMI / DVI Output */
	ret = gma3600_hdmi_init(gdev);
	if (ret)
		dev_warn(dev->dev, "HDMI output init failed: %d\n", ret);

	drm_mode_config_reset(dev);
	return 0;
}

void gma3600_display_fini(struct gma3600_device *gdev)
{
	drm_atomic_helper_shutdown(&gdev->drm);
}
