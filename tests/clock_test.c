// SPDX-License-Identifier: GPL-2.0-only
/* Run the production divider search without a GPU or a kernel module. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "gma3600_clock.h"

int main(void)
{
	static const int clocks[] = { 25175, 25200, 27000, 40000, 65000,
		74250, 85500, 108000, 148500, 162000, 225000, 297000, 355000 };
	static const int refs[] = { 27000, 96000 };
	struct gma3600_dpll_clock c;
	unsigned int i, j, checked = 0;

	for (i = 0; i < sizeof(refs) / sizeof(refs[0]); i++) {
		const struct gma3600_dpll_limits *l =
			gma3600_dpll_get_limits(GMA3600_OUTPUT_ANALOG, refs[i]);
		for (j = 0; j < sizeof(clocks) / sizeof(clocks[0]); j++) {
			assert(gma3600_dpll_find_best(refs[i], clocks[j], l, &c));
			assert(abs(c.dot - clocks[j]) <= clocks[j] / 200);
			assert(c.dot == refs[i] * (c.m2 + 2) / c.n / (c.p1 * c.p2));
			assert(c.vco >= l->vco.min && c.vco <= l->vco.max);
			assert(c.p >= l->p.min && c.p <= l->p.max);
			checked++;
		}
		assert(!gma3600_dpll_find_best(refs[i], 0, l, &c));
		assert(!gma3600_dpll_find_best(refs[i], 19999, l, &c));
		assert(!gma3600_dpll_find_best(refs[i], 400001, l, &c));
		assert(!gma3600_dpll_find_best(0, 65000, l, &c));
		assert(!gma3600_dpll_find_best(-1, 65000, l, &c));
	}
	for (i = 96000; i <= 100000; i += 4000) {
		const struct gma3600_dpll_limits *l =
			gma3600_dpll_get_limits(GMA3600_OUTPUT_LVDS, i);
		assert(gma3600_dpll_find_best(i, 65000, l, &c));
		assert(c.p2 == 14);
		assert(!gma3600_dpll_find_best(i, 148500, l, &c));
		checked++;
	}
	/* A narrow synthetic divider range cannot approximate 65 MHz. */
	{
		struct gma3600_dpll_limits l =
			*gma3600_dpll_get_limits(GMA3600_OUTPUT_ANALOG, 27000);
		l.m.min = l.m.max = 100;
		l.m2.min = l.m2.max = 98;
		l.p1.min = l.p1.max = 4;
		assert(!gma3600_dpll_find_best(27000, 65000, &l, &c));
	}
	printf("Clock regression tests passed: %u valid modes plus invalid input/tolerance cases\n", checked);
	return 0;
}
