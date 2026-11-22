// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "dpll_stubs.h"
#include "gma3600_reg.h"

static u32 mmio[0x80000 / 4], sideband[0x10000 / 4];
static bool lock_pll, busy;
static u32 fail_write;
static int resets, lane_writes;

u32 gma3600_read(struct gma3600_device *dev, u32 reg)
{
    (void)dev;
    if (reg == SB_PCKT)
        return busy ? SB_BUSY : 0;
    if ((reg == DPLL_A || reg == DPLL_B) && lock_pll &&
        (mmio[reg / 4] & DPLL_VCO_ENABLE))
        return mmio[reg / 4] | DPLL_LOCK;
    return mmio[reg / 4];
}

void gma3600_write(struct gma3600_device *dev, u32 reg, u32 value)
{
    (void)dev;
    if (reg == DPIO_CFG && value == 0) {
        /* Regression: refclk must be enabled BEFORE DPIO reset. */
        assert(mmio[DPLL_A / 4] & DPLL_SYNCLOCK_ENABLE);
        assert(!(mmio[DPLL_A / 4] & DPLL_VCO_ENABLE));
        resets++;
    }
    mmio[reg / 4] = value;
    if (reg == SB_PCKT) {
        u32 addr = mmio[SB_ADDR / 4];
        if ((value >> SB_OPCODE_SHIFT) == SB_OPCODE_WRITE) {
            if (addr == fail_write) {
                busy = true;
                return;
            }
            sideband[addr / 4] = mmio[SB_DATA / 4];
            if (addr == PSB_LANE0 || addr == PSB_LANE1)
                lane_writes++;
        } else {
            mmio[SB_DATA / 4] = sideband[addr / 4];
        }
    }
}

void gma3600_write_post(struct gma3600_device *dev, u32 reg, u32 value)
{
    gma3600_write(dev, reg, value);
    (void)gma3600_read(dev, reg);
}

void udelay(unsigned long delay) { (void)delay; }

static void reset(void)
{
    memset(mmio, 0, sizeof(mmio));
    memset(sideband, 0, sizeof(sideband));
    lock_pll = true;
    busy = false;
    fail_write = 0;
    resets = lane_writes = 0;
}

int main(void)
{
    struct gma3600_device dev = {0};
    const struct gma3600_dpll_clock clock = {
        .m2 = 98, .n = 1, .p1 = 4, .p2 = 10, .vco = 2700000,
    };
    reset();
    assert(!gma3600_dpll_program(&dev, PIPE_A, &clock, false, false));
    assert(resets == 1 && lane_writes == 0);
    assert((sideband[SB_M(PIPE_A) / 4] & SB_M_DIVIDER_MASK) == (98U << 24));
    assert((sideband[SB_P(PIPE_A) / 4] & SB_P1_DIVIDER_MASK) == (4U << 12));

    reset();
    assert(!gma3600_dpll_program(&dev, PIPE_A, &clock, false, true));
    assert(lane_writes == 2);
    assert((sideband[PSB_LANE0 / 4] & LANE_PLL_MASK) == (3U << 20));

    reset();
    lock_pll = false;
    assert(gma3600_dpll_program(&dev, PIPE_A, &clock, false, false) == -ETIMEDOUT);

    reset();
    fail_write = SB_REF_SFR(PIPE_A);
    assert(gma3600_dpll_program(&dev, PIPE_A, &clock, false, false) == -ETIMEDOUT);
    assert(!(mmio[DPLL_A / 4] & DPLL_VCO_ENABLE));

    /* Independent register-layout expectations from Linux psb_intel_reg.h. */
    assert(DSP_FIFO_SR_WM_SHIFT == 23);
    assert(DSP_FIFO_SR_WM_MASK == 0xff800000U);
    assert(CURSOR_B_FIFO_WM_SHIFT == 16);
    assert(CURSOR_B_FIFO_WM_MASK == 0x003f0000U);
    puts("MMIO regression tests passed: reset sequencing, VGA/HDMI lanes, timeout propagation, watermark layout");
    return 0;
}
