/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Intel GMA 3600 (Cedar Trail) Display Controller — Register Definitions
 *
 * The GMA 3600 is found in Intel Atom D2500/D2550/D2700/N2600/N2800
 * processors (codename Cedar Trail).  Its display controller is an Intel
 * design broadly compatible with the Gen 3/4 display register layout,
 * while the 3-D engine is a separate Imagination Technologies PowerVR
 * SGX545 core (handled by a different driver).
 *
 * Register offsets in this file are relative to PCI BAR 0 (GTTMMADR).
 * Sources: publicly released Intel datasheets, Linux kernel gma500/i915
 * open-source drivers (GPL-2.0).
 */

#ifndef __GMA3600_REG_H__
#define __GMA3600_REG_H__

/* ================================================================
 * PCI Configuration-Space Registers
 * ================================================================ */

/* IGD PCI device (device 2, function 0) */
#define GMA3600_PCI_BSM			0x5C	/* Base of Stolen Memory */

/* Host bridge (device 0, function 0) */
#define GMA3600_GMCH_CTRL		0x52
#define GMA3600_GMCH_ENABLED		(1 << 2)
#define   GMCH_GMS_MASK			0x00F0
#define   GMCH_GMS_SHIFT		4
/*
 * Cedar Trail GMS encoding (after shift):
 *   0x1 → 32 MB    0x2 → 64 MB    0x3 → 128 MB    0x4 → 256 MB
 */

/* ================================================================
 * VGA Control
 * ================================================================ */

#define VGACNTRL			0x71400
#define   VGA_DISP_DISABLE		(1u << 31)
#define   VGA_2X_MODE			(1 << 30)
#define   VGA_PIPE_B_SELECT		(1 << 29)

/* ================================================================
 * GTT / Page Table
 * ================================================================ */

#define PGETBL_CTL			0x2020
#define   PGETBL_ENABLED		(1 << 0)

/* 32-bit PTE format: PA[31:12] | VALID[0] */
#define GMA3600_PTE_VALID		(1 << 0)
#define GMA3600_PTE_CACHED		(1 << 3)

/* ================================================================
 * Clock Gating
 * ================================================================ */

#define DSPCLK_GATE_D			0x6200
#define RAMCLK_GATE_D			0x6210

/* ================================================================
 * DPLL (Display PLL) Control
 * ================================================================ */

#define DPLL_A				0x06014
#define DPLL_B				0x06018

#define   DPLL_VCO_ENABLE		(1u << 31)
#define   DPLL_DVO_HIGH_SPEED		(1 << 30)
#define   DPLL_SYNCLOCK_ENABLE		(1 << 29)
#define   DPLL_VGA_MODE_DIS		(1 << 28)
#define   DPLL_MODE_MASK		(3 << 26)
#define   DPLLB_MODE_DAC_SERIAL		(1 << 26)
#define   DPLLB_MODE_LVDS		(2 << 26)
#define   DPLL_P2_CLOCK_DIV_MASK	(1 << 24)
#define   DPLL_DAC_P2_FAST		(1 << 24)	/* P2 = 5  */
#define   DPLL_DAC_P2_SLOW		0		/* P2 = 10 */
#define   DPLL_LVDS_P2_FAST		(1 << 24)	/* P2 = 7  */
#define   DPLL_LVDS_P2_SLOW		0		/* P2 = 14 */
#define   DPLL_P1_POST_DIV_SHIFT	16
#define   DPLL_P1_POST_DIV_MASK		(0xFF << 16)
#define   DPLL_REF_CLK_SSC		0
#define   DPLL_REF_CLK_NON_SSC		(1 << 13)
#define   DPLL_LOCK			(1 << 15)	/* CDV hardware lock status */

/* CDV-specific M/D registers */
#define DPLL_A_MD			0x0601C
#define DPLL_B_MD			0x06020

/* FP (Feedback-divider) registers */
#define FPA0				0x06040
#define FPA1				0x06044
#define FPB0				0x06048
#define FPB1				0x0604C

#define   FP_N_DIV_SHIFT		16
#define   FP_N_DIV_MASK			(0x3F << 16)
#define   FP_M1_DIV_SHIFT		8		/* Always 0 on CDV */
#define   FP_M1_DIV_MASK		(0xFF << 8)
#define   FP_M2_DIV_SHIFT		0
#define   FP_M2_DIV_MASK		0xFF

/* ================================================================
 * Pipe Timing Registers  (Pipe A base 0x60000, Pipe B base 0x61000)
 * ================================================================ */

/* Pipe A */
#define HTOTAL_A			0x60000
#define HBLANK_A			0x60004
#define HSYNC_A				0x60008
#define VTOTAL_A			0x6000C
#define VBLANK_A			0x60010
#define VSYNC_A				0x60014
#define PIPEASRC			0x6001C
#define BCLRPAT_A			0x60020

/* Pipe B */
#define HTOTAL_B			0x61000
#define HBLANK_B			0x61004
#define HSYNC_B				0x61008
#define VTOTAL_B			0x6100C
#define VBLANK_B			0x61010
#define VSYNC_B				0x61014
#define PIPEBSRC			0x6101C
#define BCLRPAT_B			0x61020

/*
 * Timing-register format helpers
 *   HTOTAL / VTOTAL : [28:16] = total−1, [11:0] = active−1
 *   HBLANK / VBLANK : [28:16] = blank_end−1, [12:0] = blank_start−1
 *   HSYNC  / VSYNC  : [28:16] = sync_end−1,  [12:0] = sync_start−1
 *   PIPESRC          : [28:16] = H_size−1,    [11:0] = V_size−1
 */

/* ================================================================
 * Pipe Configuration
 * ================================================================ */

#define PIPEACONF			0x70008
#define PIPEBCONF			0x71008

#define   PIPE_ENABLE			(1u << 31)
#define   PIPE_STATE_ENABLED		(1 << 30)	/* RO — currently on */
#define   PIPE_BPC_MASK			(7 << 5)
#define   PIPE_8BPC			(0 << 5)
#define   PIPE_10BPC			(1 << 5)
#define   PIPE_6BPC			(2 << 5)
#define   PIPE_12BPC			(3 << 5)

/* Pipe event / status (second-level interrupt) */
#define PIPEASTAT			0x70024
#define PIPEBSTAT			0x71024

#define   PIPE_VBLANK_INT_EN		(1 << 17)
#define   PIPE_VBLANK_INT_STATUS	(1 << 1)
#define   PIPE_START_VBLANK_INT_EN	(1 << 18)
#define   PIPE_START_VBLANK_STATUS	(1 << 2)
#define   PIPE_CRC_DONE_INT_EN		(1 << 28)
#define   PIPE_CRC_DONE_STATUS		(1 << 12)
#define   PIPE_DPST_EVENT_INT_EN	(1 << 23)
#define   PIPE_DPST_EVENT_STATUS	(1 << 7)
#define   PIPE_FIFO_UNDERRUN_STATUS	(1 << 0)

#define   PIPE_EVENT_ENABLE_MASK	(PIPE_VBLANK_INT_EN		| \
					 PIPE_START_VBLANK_INT_EN	| \
					 PIPE_CRC_DONE_INT_EN		| \
					 PIPE_DPST_EVENT_INT_EN)

#define   PIPE_EVENT_STATUS_MASK	(PIPE_VBLANK_INT_STATUS		| \
					 PIPE_START_VBLANK_STATUS	| \
					 PIPE_CRC_DONE_STATUS		| \
					 PIPE_DPST_EVENT_STATUS		| \
					 PIPE_FIFO_UNDERRUN_STATUS)

/* ================================================================
 * Display Plane A   (base 0x70180)
 * ================================================================ */

#define DSPACNTR			0x70180
#define DSPALINOFF			0x70184
#define DSPASTRIDE			0x70188
#define DSPAPOS				0x7018C
#define DSPASIZE			0x70190
#define DSPASURF			0x7019C
#define DSPATILEOFF			0x701A4

/* ================================================================
 * Display Plane B   (base 0x71180)
 * ================================================================ */

#define DSPBCNTR			0x71180
#define DSPBLINOFF			0x71184
#define DSPBSTRIDE			0x71188
#define DSPBPOS				0x7118C
#define DSPBSIZE			0x71190
#define DSPBSURF			0x7119C
#define DSPBTILEOFF			0x711A4

/* Display-plane control bits (DSPACNTR / DSPBCNTR) */
#define   DSP_ENABLE			(1u << 31)
#define   DSP_GAMMA_ENABLE		(1 << 30)
#define   DSP_FORMAT_MASK		(0xF << 26)
#define   DSP_FORMAT_C8			(2 << 26)
#define   DSP_FORMAT_RGB565		(5 << 26)
#define   DSP_FORMAT_XRGB8888		(6 << 26)
#define   DSP_FORMAT_ARGB8888		(7 << 26)
#define   DSP_FORMAT_XBGR2101010	(8 << 26)
#define   DSP_FORMAT_XRGB2101010	(10 << 26)
#define   DSP_FORMAT_XBGR8888		(14 << 26)
#define   DSP_PIPE_SEL_B		(1 << 24)
#define   DSP_PIPE_SEL_A		0
#define   DSP_TRICKLE_FEED_DIS		(1 << 14)
#define   DSP_TILED_X			(1 << 10)

/* ================================================================
 * Hardware Cursor A / B
 * ================================================================ */

#define CURACNTR			0x70080
#define CURABASE			0x70084
#define CURAPOS				0x70088

#define CURBCNTR			0x700C0
#define CURBBASE			0x700C4
#define CURBPOS				0x700C8

/* Cursor-mode field (bits [5:0] of CUR*CNTR) */
#define   CURSOR_MODE_DISABLE		0x00
#define   CURSOR_MODE_64_32B		0x07	/* 64×64, 32 bpp, alpha */
#define   CURSOR_MODE_64_ARGB_AX	0x27	/* 64×64, 32 bpp, pre-mul α */

#define   CURSOR_GAMMA_ENABLE		(1 << 26)
#define   CURSOR_X_SIGN			(1 << 15)
#define   CURSOR_Y_SIGN			(1u << 31)

/* ================================================================
 * Output: Analog Display Port (VGA / CRT)
 * ================================================================ */

#define ADPA				0x61100

#define   ADPA_DAC_ENABLE		(1u << 31)
#define   ADPA_PIPE_SEL_MASK		(1 << 30)
#define   ADPA_PIPE_A_SELECT		0
#define   ADPA_PIPE_B_SELECT		(1 << 30)
#define   ADPA_PIPE_SEL(pipe)		((u32)(pipe) << 30)
#define   ADPA_USE_VGA_HVPOLARITY	(1 << 15)
#define   ADPA_VSYNC_CNTL_DISABLE	(1 << 11)
#define   ADPA_HSYNC_CNTL_DISABLE	(1 << 10)
#define   ADPA_DPMS_ON			(0 << 10)
#define   ADPA_DPMS_SUSPEND		(1 << 10)
#define   ADPA_DPMS_STANDBY		(2 << 10)
#define   ADPA_DPMS_OFF			(3 << 10)
#define   ADPA_DPMS_MASK		(~(3u << 10))
#define   ADPA_VSYNC_ACTIVE_HIGH	(1 << 4)
#define   ADPA_VSYNC_ACTIVE_LOW		0
#define   ADPA_HSYNC_ACTIVE_HIGH	(1 << 3)
#define   ADPA_HSYNC_ACTIVE_LOW		0
#define   ADPA_CRT_HOTPLUG_ENABLE	(1 << 23)
#define   ADPA_CRT_HOTPLUG_STATUS	(1 << 24)	/* RO */
#define   ADPA_CRT_HOTPLUG_FORCE	(1 << 16)

/* ================================================================
 * Output: LVDS
 * ================================================================ */

#define LVDS				0x61180

#define   LVDS_PORT_EN			(1u << 31)
#define   LVDS_PIPE_SEL_MASK		(1 << 30)
#define   LVDS_PIPE_A_SELECT		0
#define   LVDS_PIPE_B_SELECT		(1 << 30)
#define   LVDS_PIPE_SEL(pipe)		((u32)(pipe) << 30)
#define   LVDS_DETECTED			(1 << 1)	/* RO */
#define   LVDS_A0A2_CLKA_POWER_MASK	(3 << 8)
#define   LVDS_A0A2_CLKA_POWER_DOWN	(0 << 8)
#define   LVDS_A0A2_CLKA_POWER_UP	(3 << 8)
#define   LVDS_A3_POWER_MASK		(3 << 6)
#define   LVDS_A3_POWER_DOWN		(0 << 6)
#define   LVDS_A3_POWER_UP		(3 << 6)
#define   LVDS_CLKB_POWER_MASK		(3 << 4)
#define   LVDS_CLKB_POWER_DOWN		(0 << 4)
#define   LVDS_CLKB_POWER_UP		(3 << 4)
#define   LVDS_B0B3_POWER_MASK		(3 << 2)
#define   LVDS_B0B3_POWER_DOWN		(0 << 2)
#define   LVDS_B0B3_POWER_UP		(3 << 2)
#define   LVDS_BORDER_ENABLE		(1 << 15)

/* ================================================================
 * Output: HDMI (exposed via SDVO Port B on Cedar Trail)
 * ================================================================ */

#define HDMIB				0x61140

#define   HDMI_PORT_EN			(1u << 31)
#define   HDMI_PIPE_SEL_MASK		(1 << 30)
#define   HDMI_PIPE_A_SELECT		0
#define   HDMI_PIPE_B_SELECT		(1 << 30)
#define   HDMIB_PIPE_B_SELECT		(1 << 30)
#define   HDMI_PIPE_SEL(pipe)		((u32)(pipe) << 30)
#define   HDMI_ENCODING_TMDS		(2 << 10)
#define   HDMI_MODE_SELECT		(1 << 9)	/* 1 = HDMI, 0 = DVI */
#define   HDMI_COLOR_RANGE_16_235	(1 << 8)
#define   HDMI_AUDIO_ENABLE		(1 << 6)
#define   HDMI_DETECTED			(1 << 2)	/* RO */
#define   HDMI_VSYNC_ACTIVE_HIGH	(1 << 4)
#define   HDMI_HSYNC_ACTIVE_HIGH	(1 << 3)
#define   HDMI_NULL_PKT_VSYNC		(1 << 9)
#define   HDMI_BORDER_ENABLE		(1 << 7)

/* ================================================================
 * Panel Power Sequencing
 * ================================================================ */

#define PP_STATUS			0x61200
#define   PP_ON				(1u << 31)	/* RO */
#define PP_CONTROL			0x61204
#define   POWER_TARGET_ON		(1 << 0)
#define PP_ON_DELAYS			0x61208
#define PP_OFF_DELAYS			0x6120C
#define PP_DIVISOR			0x61210

/* ================================================================
 * Backlight PWM
 * ================================================================ */

#define BLC_PWM_CTL2			0x61250
#define   BLM_PWM_ENABLE		(1u << 31)
#define   BLM_COMBINATION_MODE		(1 << 30)
#define   BLM_PIPE_SELECT		(1 << 29)	/* 0 = pipe A */
#define   BLM_POLARITY_NORMAL		(0 << 28)
#define   BLM_POLARITY_INVERTED		(1 << 28)

#define BLC_PWM_CTL			0x61254
/* [31:16] = max (modulation frequency), [15:0] = duty cycle */
#define   BL_DUTY_SHIFT			0
#define   BL_DUTY_MASK			0xFFFF
#define   BL_MAX_SHIFT			16
#define   BL_MAX_MASK			(0xFFFFu << 16)

/* ================================================================
 * GMBUS (I²C / DDC)
 * ================================================================ */

#define GMBUS0				0x5100	/* Clock / Port Select */
#define GMBUS1				0x5104	/* Command / Status    */
#define GMBUS2				0x5108	/* Status              */
#define GMBUS3				0x510C	/* Data Buffer         */
#define GMBUS4				0x5110	/* Interrupt Mask      */
#define GMBUS5				0x5120	/* 2-byte Index        */

/* GMBUS0 */
#define   GMBUS_RATE_MASK		(3 << 8)
#define   GMBUS_RATE_100KHZ		(0 << 8)
#define   GMBUS_RATE_50KHZ		(1 << 8)
#define   GMBUS_RATE_400KHZ		(2 << 8)
#define   GMBUS_RATE_1MHZ		(3 << 8)
#define   GMBUS_HOLD_EXT		(1 << 7)
#define   GMBUS_PIN_MASK		0x07
#define   GMBUS_PIN_DISABLED		0
#define   GMBUS_PIN_SSC			1
#define   GMBUS_PIN_VGADDC		2
#define   GMBUS_PIN_PANEL		3
#define   GMBUS_PIN_DPB			4
#define   GMBUS_PIN_DPC			5
#define   GMBUS_PIN_DPD			6

/* GMBUS1 */
#define   GMBUS_SW_CLR_INT		(1u << 31)
#define   GMBUS_SW_RDY			(1 << 30)
#define   GMBUS_ENT			(1 << 29)
#define   GMBUS_CYCLE_NONE		(0 << 25)
#define   GMBUS_CYCLE_WAIT		(1 << 25)
#define   GMBUS_CYCLE_INDEX		(2 << 25)
#define   GMBUS_CYCLE_STOP		(4 << 25)
#define   GMBUS_BYTE_COUNT_SHIFT	16
#define   GMBUS_BYTE_COUNT_MAX		256
#define   GMBUS_SLAVE_ADDR_SHIFT	1
#define   GMBUS_SLAVE_READ		1
#define   GMBUS_SLAVE_WRITE		0

/* GMBUS2 */
#define   GMBUS_INUSE			(1 << 15)
#define   GMBUS_HW_WAIT_PHASE		(1 << 14)
#define   GMBUS_STALL_TIMEOUT		(1 << 13)
#define   GMBUS_INT_STATUS		(1 << 12)
#define   GMBUS_HW_RDY			(1 << 11)
#define   GMBUS_SATOER			(1 << 10)
#define   GMBUS_ACTIVE			(1 << 9)

/* GPIO registers (bit-bang fallback for I²C) */
#define GPIOA				0x5010
#define GPIOB				0x5014
#define GPIOC				0x5018
#define GPIOD				0x501C
#define GPIOE				0x5020
#define GPIOF				0x5024

#define   GPIO_CLOCK_DIR_MASK		(1 << 0)
#define   GPIO_CLOCK_DIR_IN		0
#define   GPIO_CLOCK_DIR_OUT		(1 << 1)
#define   GPIO_CLOCK_VAL_MASK		(1 << 2)
#define   GPIO_CLOCK_VAL_OUT		(1 << 3)
#define   GPIO_CLOCK_VAL_IN		(1 << 4)
#define   GPIO_DATA_DIR_MASK		(1 << 8)
#define   GPIO_DATA_DIR_IN		0
#define   GPIO_DATA_DIR_OUT		(1 << 9)
#define   GPIO_DATA_VAL_MASK		(1 << 10)
#define   GPIO_DATA_VAL_OUT		(1 << 11)
#define   GPIO_DATA_VAL_IN		(1 << 12)

#define GPIO_CLOCK_PULLUP_DISABLE (1 << 5)
#define GPIO_DATA_PULLUP_DISABLE (1 << 13)

#define PORT_HOTPLUG_EN 0x61110
#define PORT_HOTPLUG_STAT 0x61114
#define CRT_HOTPLUG_FORCE_DETECT (1 << 3)
#define CRT_HOTPLUG_DETECT_MASK 0xf8
#define CRT_HOTPLUG_ACTIVATION_PERIOD_64 (1 << 8)
#define CRT_HOTPLUG_VOLTAGE_COMPARE_50 (1 << 5)
#define CRT_HOTPLUG_INT_STATUS (1 << 11)
#define CRT_HOTPLUG_MONITOR_MASK (3 << 8)

/* ================================================================
 * Top-Level Interrupts
 * ================================================================ */

#define IER				0x020A0
#define IIR				0x020A4
#define IMR				0x020A8
#define ISR				0x020AC

/* Bits in IER / IIR / IMR / ISR for Cedar Trail */
#define   DE_MASTER_IRQ_CONTROL		(1u << 31)
#define   DE_HOTPLUG			(1 << 17)
#define   DE_PORT_EVENT			(1 << 20)
#define   DE_PIPEA_EVENT		(1 << 7)
#define   DE_PIPEB_EVENT		(1 << 5)
#define   DE_PIPEA_VBLANK		(1 << 7)
#define   DE_PIPEB_VBLANK		(1 << 5)
#define   DE_PIPE_EVENT(pipe)		((pipe) == 0 ? DE_PIPEA_EVENT \
						      : DE_PIPEB_EVENT)

/* ================================================================
 * DPIO Side-band (CDV PLL-PHY Configuration)
 * ================================================================ */

#define SB_PCKT				0x02100
#define   SB_BUSY			(1 << 0)
#define   SB_BYTE_ENABLE_SHIFT		4
#define   SB_BYTE_ENABLE_MASK		(0xFu << 4)
#define   SB_DEST_SHIFT			8
#define   SB_DEST_MASK			(0xFFu << 8)
#define   SB_DEST_DPLL			0x88
#define   SB_OPCODE_SHIFT		16
#define   SB_OPCODE_MASK		(0xFFFFu << 16)
#define   SB_OPCODE_READ		0
#define   SB_OPCODE_WRITE		1

#define SB_DATA				0x02104
#define SB_ADDR				0x02108

#define DPIO_CFG			0x02110
#define   DPIO_MODE_SELECT_1		(1 << 3)
#define   DPIO_MODE_SELECT_0		(1 << 2)
#define   DPIO_SFR_BYPASS		(1 << 1)
#define   DPIO_CMN_RESET_N		(1 << 0)

/* Cedarview DPIO sideband registers */
#define _SB_M_A				0x8008
#define _SB_M_B				0x8028
#define SB_M(pipe)			((pipe) == 0 ? _SB_M_A : _SB_M_B)
#define   SB_M_DIVIDER_SHIFT		24
#define   SB_M_DIVIDER_MASK		(0xFFu << 24)

#define _SB_N_VCO_A			0x8014
#define _SB_N_VCO_B			0x8034
#define SB_N_VCO(pipe)			((pipe) == 0 ? _SB_N_VCO_A : _SB_N_VCO_B)
#define   SB_N_VCO_SEL_SHIFT		30
#define   SB_N_VCO_SEL_MASK		(0x3u << 30)
#define   SB_N_DIVIDER_SHIFT		26
#define   SB_N_DIVIDER_MASK		(0xFu << 26)
#define   SB_N_CB_TUNE_SHIFT		24
#define   SB_N_CB_TUNE_MASK		(0x3u << 24)

#define SB_REF_DPLLA			0x8010
#define SB_REF_DPLLB			0x8030
#define   REF_CLK_MASK			(0x3u << 13)
#define   REF_CLK_CORE			(0x0u << 13)
#define   REF_CLK_DPLL			(0x1u << 13)
#define   REF_CLK_DPLLA			(0x2u << 13)

#define _SB_REF_A			0x8018
#define _SB_REF_B			0x8038
#define SB_REF_SFR(pipe)		((pipe) == 0 ? _SB_REF_A : _SB_REF_B)

#define _SB_P_A				0x801C
#define _SB_P_B				0x803C
#define SB_P(pipe)			((pipe) == 0 ? _SB_P_A : _SB_P_B)
#define   SB_P2_DIVIDER_SHIFT		30
#define   SB_P2_DIVIDER_MASK		(0x3u << 30)
#define   SB_P2_10			0u
#define   SB_P2_5			1u
#define   SB_P2_14			2u
#define   SB_P2_7			3u
#define   SB_P1_DIVIDER_SHIFT		12
#define   SB_P1_DIVIDER_MASK		(0xFu << 12)

#define PSB_LANE0			0x0120
#define PSB_LANE1			0x0220
#define PSB_LANE2			0x2320
#define PSB_LANE3			0x2420
#define   LANE_PLL_MASK			(0x7u << 20)
#define   LANE_PLL_ENABLE		(0x3u << 20)
#define   LANE_PLL_PIPE(p)		((p) == PIPE_A ? (1u << 21) : 0)

/* ================================================================
 * Fence Registers (X-tiling)
 * ================================================================ */

#define FENCE_REG_CDV_BASE		0x03000
#define FENCE_REG_COUNT			16
#define FENCE_REG(i)			(FENCE_REG_CDV_BASE + (i) * 8)

/* ================================================================
 * FIFO Watermarks / Arbitration
 * ================================================================ */

#define FW_BLC_SELF			0x20E0
#define   FW_BLC_SELF_EN		(1 << 15)

#define DSPARB				0x70030
#define DSPFW1				0x70034
#define DSP_FIFO_SR_WM_MASK 0xff800000U
#define DSP_FIFO_SR_WM_SHIFT 23
#define CURSOR_B_FIFO_WM_MASK 0x003f0000U
#define CURSOR_B_FIFO_WM_SHIFT 16
#define DSPFW2				0x70038
#define DSPFW3				0x7003C
#define DSPFW4				0x70050
#define DSPFW5				0x70054
#define DSPFW6				0x70058

/* ================================================================
 * Palette / Gamma LUT  (256 × 32-bit entries per pipe)
 * ================================================================ */

#define PALETTE_A			0x0A000
#define PALETTE_B			0x0A800

/* ================================================================
 * Pipe-Indexed Register Helpers
 * ================================================================ */

#define _PIPE(pipe, a, b)		((pipe) == 0 ? (a) : (b))
#define HTOTAL(pipe)			_PIPE(pipe, HTOTAL_A, HTOTAL_B)
#define HBLANK(pipe)			_PIPE(pipe, HBLANK_A, HBLANK_B)
#define HSYNC(pipe)			_PIPE(pipe, HSYNC_A, HSYNC_B)
#define VTOTAL(pipe)			_PIPE(pipe, VTOTAL_A, VTOTAL_B)
#define VBLANK(pipe)			_PIPE(pipe, VBLANK_A, VBLANK_B)
#define VSYNC(pipe)			_PIPE(pipe, VSYNC_A, VSYNC_B)
#define PIPESRC(pipe)			_PIPE(pipe, PIPEASRC, PIPEBSRC)
#define PIPECONF(pipe)			_PIPE(pipe, PIPEACONF, PIPEBCONF)
#define PIPESTAT(pipe)			_PIPE(pipe, PIPEASTAT, PIPEBSTAT)
#define DSPCNTR(pipe)			_PIPE(pipe, DSPACNTR, DSPBCNTR)
#define DSPLINOFF(pipe)			_PIPE(pipe, DSPALINOFF, DSPBLINOFF)
#define DSPSTRIDE(pipe)			_PIPE(pipe, DSPASTRIDE, DSPBSTRIDE)
#define DSPPOS(pipe)			_PIPE(pipe, DSPAPOS, DSPBPOS)
#define DSPSIZE(pipe)			_PIPE(pipe, DSPASIZE, DSPBSIZE)
#define DSPSURF(pipe)			_PIPE(pipe, DSPASURF, DSPBSURF)
#define DSPTILEOFF(pipe)		_PIPE(pipe, DSPATILEOFF, DSPBTILEOFF)
#define CURCNTR(pipe)			_PIPE(pipe, CURACNTR, CURBCNTR)
#define CURBASE(pipe)			_PIPE(pipe, CURABASE, CURBBASE)
#define CURPOS(pipe)			_PIPE(pipe, CURAPOS, CURBPOS)
#define BCLRPAT(pipe)			_PIPE(pipe, BCLRPAT_A, BCLRPAT_B)
#define DPLL(pipe)			_PIPE(pipe, DPLL_A, DPLL_B)
#define DPLL_MD(pipe)			_PIPE(pipe, DPLL_A_MD, DPLL_B_MD)
#define FP0(pipe)			_PIPE(pipe, FPA0, FPB0)
#define FP1(pipe)			_PIPE(pipe, FPA1, FPB1)
#define PALETTE(pipe)			_PIPE(pipe, PALETTE_A, PALETTE_B)

#endif /* __GMA3600_REG_H__ */
