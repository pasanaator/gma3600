// SPDX-License-Identifier: GPL-2.0-only
/* Pure divider search, shared by the driver and the host regression tests. */
#ifdef __KERNEL__
#include <linux/kernel.h>
#include <linux/string.h>
#else
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#endif
#include "gma3600_clock.h"

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

	if (refclk <= 0 || refclk > 100000 || target < limits->dot.min ||
	    target > limits->dot.max)
		return false;

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

					if (clock.m < limits->m.min || clock.m > limits->m.max ||
					    clock.p < limits->p.min || clock.p > limits->p.max)
						continue;
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

	/* Reject clocks more than 0.5% away instead of silently sending bad timings. */
	return found && min_err <= target / 200;
}

