// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel GMA 3600 (Cedar Trail) DRM/KMS Driver — DPLL Clock Synthesis
 *
 * Copyright (C) 2026 Antigravity Project
 *
 * Cedarview (CDV) DPLL hardware architecture:
 * Unlike legacy Intel Gen 3/4 graphics engines, the Cedarview DPLL divider
 * and feedback registers (M, N, P, reference clock routing) reside behind
 * the indirect DPIO Sideband (SB) packet interface (MMIO 0x02100).
 *
 * Timing register setup requires:
 * 1. DPIO reset via DPIO_CFG (0x02110).
 * 2. Enabling DPLL synclock in DPLL_A/B (0x06014/0x06018).
 * 3. Programming DPIO registers: SB_REF_SFR, SB_REF_DPLL*, SB_M, SB_N_VCO, SB_P.
 * 4. Enabling DPIO TMDS PHY lanes (PSB_LANE0/1) for HDMI.
 * 5. Latching VCO enable and waiting for hardware DPLL_LOCK (bit 15).
 */

#include <linux/delay.h>
#include <linux/kernel.h>

#include "gma3600_drv.h"
#include "gma3600_reg.h"

/* ================================================================
 * DPIO Side-band Packet Access Helpers
 * ================================================================ */

int gma3600_sb_read(struct gma3600_device *gdev, u32 reg, u32 *val)
{
	u32 pckt;
	int ret;

	ret = read_poll_timeout_atomic(gma3600_read, pckt, !(pckt & SB_BUSY),
				       10, 10000, false, gdev, SB_PCKT);
	if (ret) {
		dev_err(gdev->drm.dev, "timeout waiting for SB to idle before read (reg 0x%04x)\n", reg);
		return ret;
	}

	gma3600_write(gdev, SB_ADDR, reg);
	gma3600_write(gdev, SB_PCKT,
		      (SB_OPCODE_READ << SB_OPCODE_SHIFT) |
		      (SB_DEST_DPLL << SB_DEST_SHIFT) |
		      (0xF << SB_BYTE_ENABLE_SHIFT));

	ret = read_poll_timeout_atomic(gma3600_read, pckt, !(pckt & SB_BUSY),
				       10, 10000, false, gdev, SB_PCKT);
	if (ret) {
		dev_err(gdev->drm.dev, "timeout waiting for SB to idle after read (reg 0x%04x)\n", reg);
		return ret;
	}

	*val = gma3600_read(gdev, SB_DATA);
	return 0;
}

int gma3600_sb_write(struct gma3600_device *gdev, u32 reg, u32 val)
{
	u32 pckt;
	int ret;

	ret = read_poll_timeout_atomic(gma3600_read, pckt, !(pckt & SB_BUSY),
				       10, 10000, false, gdev, SB_PCKT);
	if (ret) {
		dev_err(gdev->drm.dev, "timeout waiting for SB to idle before write (reg 0x%04x)\n", reg);
		return ret;
	}

	gma3600_write(gdev, SB_ADDR, reg);
	gma3600_write(gdev, SB_DATA, val);
	gma3600_write(gdev, SB_PCKT,
		      (SB_OPCODE_WRITE << SB_OPCODE_SHIFT) |
		      (SB_DEST_DPLL << SB_DEST_SHIFT) |
		      (0xF << SB_BYTE_ENABLE_SHIFT));

	ret = read_poll_timeout_atomic(gma3600_read, pckt, !(pckt & SB_BUSY),
				       10, 10000, false, gdev, SB_PCKT);
	if (ret) {
		dev_err(gdev->drm.dev, "timeout waiting for SB to idle after write (reg 0x%04x)\n", reg);
		return ret;
	}

	return 0;
}

void gma3600_sb_reset(struct gma3600_device *gdev)
{
	gma3600_write(gdev, DPIO_CFG, 0);
	gma3600_read(gdev, DPIO_CFG);
	gma3600_write(gdev, DPIO_CFG, DPIO_MODE_SELECT_0 | DPIO_CMN_RESET_N);
}

/* ================================================================
 * DPLL Frequency Synthesis Limits for Cedarview
 * ================================================================ */

/* 27 MHz reference clock (Desktop Cedarview SKUs: D2500 / D2550 / D2700 on CRT/HDMI) */
static const struct gma3600_dpll_limits cdv_limits_dac_27 = {
	.dot = { .min = 20000,   .max = 400000 },
	.vco = { .min = 1809000, .max = 3564000 },
	.n   = { .min = 1,       .max = 1 },
	.m   = { .min = 67,      .max = 132 },
	.m1  = { .min = 0,       .max = 0 },
	.m2  = { .min = 65,      .max = 130 },
	.p   = { .min = 5,       .max = 90 },
	.p1  = { .min = 1,       .max = 9 },
	.p2  = { .dot_limit = 225000, .p2_slow = 10, .p2_fast = 5 },
};

/* 96 MHz reference clock (Netbook SKUs: N2600 / N2800, or platforms with primary LFP) */
static const struct gma3600_dpll_limits cdv_limits_dac_96 = {
	.dot = { .min = 20000,   .max = 400000 },
	.vco = { .min = 1800000, .max = 3600000 },
	.n   = { .min = 2,       .max = 6 },
	.m   = { .min = 60,      .max = 160 },
	.m1  = { .min = 0,       .max = 0 },
	.m2  = { .min = 58,      .max = 158 },
	.p   = { .min = 5,       .max = 100 },
	.p1  = { .min = 1,       .max = 10 },
	.p2  = { .dot_limit = 225000, .p2_slow = 10, .p2_fast = 5 },
};

/* Single-link LVDS (96 MHz reference clock) */
static const struct gma3600_dpll_limits cdv_limits_lvds_single_96 = {
	.dot = { .min = 20000,   .max = 115500 },
	.vco = { .min = 1800000, .max = 3600000 },
	.n   = { .min = 2,       .max = 6 },
	.m   = { .min = 60,      .max = 160 },
	.m1  = { .min = 0,       .max = 0 },
	.m2  = { .min = 58,      .max = 158 },
	.p   = { .min = 28,      .max = 140 },
	.p1  = { .min = 2,       .max = 10 },
	.p2  = { .dot_limit = 200000, .p2_slow = 14, .p2_fast = 14 },
};

/* Single-link LVDS (100 MHz SSC reference clock) */
static const struct gma3600_dpll_limits cdv_limits_lvds_single_100 = {
	.dot = { .min = 20000,   .max = 115500 },
	.vco = { .min = 1800000, .max = 3600000 },
	.n   = { .min = 2,       .max = 6 },
	.m   = { .min = 60,      .max = 160 },
	.m1  = { .min = 0,       .max = 0 },
	.m2  = { .min = 58,      .max = 158 },
	.p   = { .min = 28,      .max = 140 },
	.p1  = { .min = 2,       .max = 10 },
	.p2  = { .dot_limit = 200000, .p2_slow = 14, .p2_fast = 14 },
};

const struct gma3600_dpll_limits *
gma3600_dpll_get_limits(enum gma3600_output_type type, int refclk)
{
	if (type == GMA3600_OUTPUT_LVDS) {
		if (refclk == 100000)
			return &cdv_limits_lvds_single_100;
		return &cdv_limits_lvds_single_96;
	}

	if (refclk == 27000)
		return &cdv_limits_dac_27;

	return &cdv_limits_dac_96;
}

static void gma3600_dpll_calc(int refclk, struct gma3600_dpll_clock *clock)
{
	clock->m = clock->m2 + 2;
	clock->p = clock->p1 * clock->p2;
	if (clock->n == 0 || clock->p == 0)
		return;

	clock->vco = (refclk * clock->m) / clock->n;
	clock->dot = clock->vco / clock->p;
}

bool gma3600_dpll_find_best(int refclk, int target,
			    const struct gma3600_dpll_limits *limits,
			    struct gma3600_dpll_clock *best)
{
	struct gma3600_dpll_clock clock;
	int min_err = INT_MAX;
	bool found = false;

	memset(&clock, 0, sizeof(clock));
	memset(best, 0, sizeof(*best));

	if (target > limits->p2.dot_limit && limits->p2.dot_limit != 0)
		clock.p2 = limits->p2.p2_fast;
	else
		clock.p2 = limits->p2.p2_slow;

	for (clock.m1 = limits->m1.min; clock.m1 <= limits->m1.max; clock.m1++) {
		for (clock.m2 = limits->m2.min; clock.m2 <= limits->m2.max; clock.m2++) {
			for (clock.n = limits->n.min; clock.n <= limits->n.max; clock.n++) {
				for (clock.p1 = limits->p1.min; clock.p1 <= limits->p1.max; clock.p1++) {
					int err;

					gma3600_dpll_calc(refclk, &clock);

					if (clock.vco < limits->vco.min || clock.vco > limits->vco.max)
						continue;
					if (clock.dot < limits->dot.min || clock.dot > limits->dot.max)
						continue;

					err = abs(clock.dot - target);
					if (err < min_err) {
						min_err = err;
						*best = clock;
						found = true;
						if (err == 0)
							return true;
					}
				}
			}
		}
	}

	return found;
}

int gma3600_dpll_program(struct gma3600_device *gdev,
			 enum gma3600_pipe pipe,
			 const struct gma3600_dpll_clock *clock,
			 bool is_lvds,
			 bool is_hdmi)
{
	u32 reg_dpll = (pipe == PIPE_A) ? DPLL_A : DPLL_B;
	u32 ref_sfr  = (pipe == PIPE_A) ? SB_REF_DPLLA : SB_REF_DPLLB;
	u32 dpll, m, n_vco, p, ref_val, lane_val;
	u32 dpll_mode = DPLL_SYNCLOCK_ENABLE | DPLL_VGA_MODE_DIS;
	int ret;

	/* Select operating mode bits for DPLL */
	if (is_lvds)
		dpll_mode |= DPLLB_MODE_LVDS;
	else if (is_hdmi)
		dpll_mode |= DPLL_DVO_HIGH_SPEED | DPLLB_MODE_DAC_SERIAL;
	else
		dpll_mode |= DPLLB_MODE_DAC_SERIAL;

	/* 1. Reset DPIO indirect bus */
	gma3600_sb_reset(gdev);

	/* 2. Configure DPLL control register with synclock & output serializer mode */
	gma3600_write(gdev, reg_dpll, dpll_mode);
	gma3600_read(gdev, reg_dpll);
	udelay(100);

	/* Reset pixel multiplier in DPLL_MD to 1x before enabling VCO */
	gma3600_write(gdev, DPLL_MD(pipe), 0);
	gma3600_read(gdev, DPLL_MD(pipe));

	/* 3. Write hardcoded reference SFR register (0x68A701) */
	gma3600_sb_write(gdev, SB_REF_SFR(pipe), 0x68A701);

	/* 4. Configure reference clock selection in ref_sfr */
	ret = gma3600_sb_read(gdev, ref_sfr, &ref_val);
	if (ret)
		return ret;
	ref_val &= ~REF_CLK_MASK;
	ref_val |= REF_CLK_DPLL;
	ret = gma3600_sb_write(gdev, ref_sfr, ref_val);
	if (ret)
		return ret;

	/* 5. Program M2 feedback divider in SB_M(pipe) */
	ret = gma3600_sb_read(gdev, SB_M(pipe), &m);
	if (ret)
		return ret;
	m &= ~SB_M_DIVIDER_MASK;
	m |= ((clock->m2 & 0xFF) << SB_M_DIVIDER_SHIFT);
	ret = gma3600_sb_write(gdev, SB_M(pipe), m);
	if (ret)
		return ret;

	/* 6. Program N divider and VCO tuning band in SB_N_VCO(pipe) */
	ret = gma3600_sb_read(gdev, SB_N_VCO(pipe), &n_vco);
	if (ret)
		return ret;
	n_vco &= 0xFFFF;
	n_vco |= 0x107;
	n_vco &= ~(SB_N_VCO_SEL_MASK | SB_N_DIVIDER_MASK | SB_N_CB_TUNE_MASK);
	n_vco |= ((clock->n & 0xF) << SB_N_DIVIDER_SHIFT);

	if (clock->vco < 2250000) {
		n_vco |= (2U << SB_N_CB_TUNE_SHIFT) | (0U << SB_N_VCO_SEL_SHIFT);
	} else if (clock->vco < 2750000) {
		n_vco |= (1U << SB_N_CB_TUNE_SHIFT) | (1U << SB_N_VCO_SEL_SHIFT);
	} else if (clock->vco < 3300000) {
		n_vco |= (0U << SB_N_CB_TUNE_SHIFT) | (2U << SB_N_VCO_SEL_SHIFT);
	} else {
		n_vco |= (0U << SB_N_CB_TUNE_SHIFT) | (3U << SB_N_VCO_SEL_SHIFT);
	}
	ret = gma3600_sb_write(gdev, SB_N_VCO(pipe), n_vco);
	if (ret)
		return ret;

	/* 7. Program P1 and P2 post-dividers in SB_P(pipe) */
	ret = gma3600_sb_read(gdev, SB_P(pipe), &p);
	if (ret)
		return ret;
	p &= ~(SB_P2_DIVIDER_MASK | SB_P1_DIVIDER_MASK);
	p |= ((clock->p1 & 0xF) << SB_P1_DIVIDER_SHIFT);

	switch (clock->p2) {
	case 5:
		p |= (SB_P2_5 << SB_P2_DIVIDER_SHIFT);
		break;
	case 10:
		p |= (SB_P2_10 << SB_P2_DIVIDER_SHIFT);
		break;
	case 14:
		p |= (SB_P2_14 << SB_P2_DIVIDER_SHIFT);
		break;
	case 7:
		p |= (SB_P2_7 << SB_P2_DIVIDER_SHIFT);
		break;
	default:
		dev_err(gdev->drm.dev, "Invalid P2 divider: %d\n", clock->p2);
		return -EINVAL;
	}
	ret = gma3600_sb_write(gdev, SB_P(pipe), p);
	if (ret)
		return ret;

	/* 8. Enable DPIO PHY display lanes (LANE0 and LANE1) for the pipe */
	gma3600_sb_read(gdev, PSB_LANE0, &lane_val);
	lane_val &= ~LANE_PLL_MASK;
	lane_val |= LANE_PLL_ENABLE | LANE_PLL_PIPE(pipe);
	gma3600_sb_write(gdev, PSB_LANE0, lane_val);

	gma3600_sb_read(gdev, PSB_LANE1, &lane_val);
	lane_val &= ~LANE_PLL_MASK;
	lane_val |= LANE_PLL_ENABLE | LANE_PLL_PIPE(pipe);
	gma3600_sb_write(gdev, PSB_LANE1, lane_val);

	udelay(150);

	/* 9. Enable VCO and latch twice per hardware errata */
	dpll = dpll_mode | DPLL_VCO_ENABLE;
	gma3600_write(gdev, reg_dpll, dpll);
	gma3600_read(gdev, reg_dpll);
	udelay(150);
	gma3600_write(gdev, reg_dpll, dpll);
	gma3600_read(gdev, reg_dpll);
	udelay(150);

	/* 10. Wait for hardware DPLL lock bit (bit 15) */
	ret = read_poll_timeout_atomic(gma3600_read, dpll, (dpll & DPLL_LOCK),
				       10, 15000, false, gdev, reg_dpll);
	if (ret) {
		dev_warn(gdev->drm.dev, "DPLL %c did not lock (val=0x%08x)\n",
			 'A' + pipe, dpll);
		return -ETIMEDOUT;
	}

	return 0;
}

void gma3600_dpll_enable(struct gma3600_device *gdev, enum gma3600_pipe pipe)
{
	u32 reg_dpll = (pipe == PIPE_A) ? DPLL_A : DPLL_B;
	u32 val = gma3600_read(gdev, reg_dpll);

	val |= DPLL_VCO_ENABLE;
	gma3600_write_post(gdev, reg_dpll, val);
	udelay(150);
}

void gma3600_dpll_disable(struct gma3600_device *gdev, enum gma3600_pipe pipe)
{
	u32 reg_dpll = (pipe == PIPE_A) ? DPLL_A : DPLL_B;
	u32 val = gma3600_read(gdev, reg_dpll);

	val &= ~DPLL_VCO_ENABLE;
	gma3600_write_post(gdev, reg_dpll, val);
	udelay(150);
}
