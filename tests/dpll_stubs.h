/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA3600_DPLL_TEST_STUBS_H
#define GMA3600_DPLL_TEST_STUBS_H
/* Compile the production MMIO implementation with deterministic fake hardware. */
#define __GMA3600_DRV_H__
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include "gma3600_clock.h"
typedef uint32_t u32;
enum gma3600_pipe { PIPE_A, PIPE_B };
struct gma3600_device { int unused; };
u32 gma3600_read(struct gma3600_device *dev, u32 reg);
void gma3600_write(struct gma3600_device *dev, u32 reg, u32 value);
void gma3600_write_post(struct gma3600_device *dev, u32 reg, u32 value);
void udelay(unsigned long delay);
#define dev_err(...) ((void)0)
#define dev_warn(...) ((void)0)
/* Each tested device reaches its terminal state immediately. */
#define read_poll_timeout_atomic(op, val, cond, delay, timeout, before, ...) \
    ({ (val) = op(__VA_ARGS__); (cond) ? 0 : -ETIMEDOUT; })
int gma3600_dpll_program(struct gma3600_device *dev, enum gma3600_pipe pipe,
    const struct gma3600_dpll_clock *clock, bool lvds, bool hdmi);
#endif
