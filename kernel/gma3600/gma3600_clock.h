/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA3600_CLOCK_H
#define GMA3600_CLOCK_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#endif

enum gma3600_output_type {
	GMA3600_OUTPUT_UNUSED = 0,
	GMA3600_OUTPUT_ANALOG,		/* VGA / CRT */
	GMA3600_OUTPUT_LVDS,
	GMA3600_OUTPUT_HDMI,
};

struct gma3600_dpll_clock {
	int n;
	int m1;		/* Always 0 on CDV */
	int m2;
	int m;		/* = m2 + 2             */
	int p1;
	int p2;
	int p;		/* = p1 × p2            */
	int vco;	/* kHz                  */
	int dot;	/* Pixel-clock in kHz   */
};

struct gma3600_dpll_range {
	int min;
	int max;
};

struct gma3600_dpll_p2 {
	int dot_limit;	/* kHz boundary between fast/slow P2 */
	int p2_slow;
	int p2_fast;
};

struct gma3600_dpll_limits {
	struct gma3600_dpll_range dot;
	struct gma3600_dpll_range vco;
	struct gma3600_dpll_range n;
	struct gma3600_dpll_range m;
	struct gma3600_dpll_range m1;
	struct gma3600_dpll_range m2;
	struct gma3600_dpll_range p;
	struct gma3600_dpll_range p1;
	struct gma3600_dpll_p2   p2;
};

bool gma3600_dpll_find_best(int refclk, int target,
	const struct gma3600_dpll_limits *limits, struct gma3600_dpll_clock *best);
const struct gma3600_dpll_limits *gma3600_dpll_get_limits(
	enum gma3600_output_type type, int refclk);
#endif
