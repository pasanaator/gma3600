/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Intel GMA 3600 (Cedar Trail) DRM/KMS Driver — Main Header
 *
 * Data structures, inline helpers, and function declarations used
 * across every translation unit of the gma3600 kernel module.
 */

#ifndef __GMA3600_DRV_H__
#define __GMA3600_DRV_H__

#include <linux/pci.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/backlight.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/bitmap.h>

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_crtc.h>
#include <drm/drm_plane.h>
#include <drm/drm_encoder.h>
#include <drm/drm_connector.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_gem.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <linux/version.h>

#ifndef DRM_PLANE_NO_SCALING
#define DRM_PLANE_NO_SCALING (1 << 16)
#endif

#ifndef read_poll_timeout_atomic
#define read_poll_timeout_atomic(op, val, cond, delay_us, timeout_us, delay_before_read, args...) \
({ \
	u64 __timeout_us = (timeout_us); \
	unsigned long __delay_us = (delay_us); \
	ktime_t __timeout = ktime_add_us(ktime_get(), __timeout_us); \
	if (delay_before_read && __delay_us) \
		udelay(__delay_us); \
	for (;;) { \
		(val) = op(args); \
		if (cond) \
			break; \
		if (__timeout_us && \
		    ktime_compare(ktime_get(), __timeout) > 0) { \
			(val) = op(args); \
			break; \
		} \
		if (__delay_us) \
			udelay(__delay_us); \
	} \
	(cond) ? 0 : -ETIMEDOUT; \
})
#endif
#include <drm/drm_managed.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_file.h>

#include "gma3600_reg.h"

/* ================================================================
 * Version / identity
 * ================================================================ */

#define DRIVER_NAME		"gma3600"
#define DRIVER_DESC		"Intel GMA 3600 (Cedar Trail)"
#define DRIVER_DATE		"20261121"
#define DRIVER_MAJOR		1
#define DRIVER_MINOR		0
#define DRIVER_PATCHLEVEL	0

/* ================================================================
 * Compile-time constants
 * ================================================================ */

#define GMA3600_MAX_PIPES	2	/* Pipe A, Pipe B               */
#define GMA3600_GTT_PAGE_SIZE	4096
#define GMA3600_CURSOR_W	64
#define GMA3600_CURSOR_H	64

/* Cedar Trail DPLL reference clocks (kHz) */
#define GMA3600_REF_CLK_96	96000
#define GMA3600_REF_CLK_100	100000
#define GMA3600_DEFAULT_REF_CLK	GMA3600_REF_CLK_96

/* PCI device IDs — 0x0BE0 … 0x0BEF */
#define PCI_DEVICE_ID_CDV_0	0x0BE0
#define PCI_DEVICE_ID_CDV_1	0x0BE1
#define PCI_DEVICE_ID_CDV_2	0x0BE2
#define PCI_DEVICE_ID_CDV_3	0x0BE3
#define PCI_DEVICE_ID_CDV_4	0x0BE4
#define PCI_DEVICE_ID_CDV_5	0x0BE5
#define PCI_DEVICE_ID_CDV_6	0x0BE6
#define PCI_DEVICE_ID_CDV_7	0x0BE7
#define PCI_DEVICE_ID_CDV_8	0x0BE8
#define PCI_DEVICE_ID_CDV_9	0x0BE9
#define PCI_DEVICE_ID_CDV_A	0x0BEA
#define PCI_DEVICE_ID_CDV_B	0x0BEB
#define PCI_DEVICE_ID_CDV_C	0x0BEC
#define PCI_DEVICE_ID_CDV_D	0x0BED
#define PCI_DEVICE_ID_CDV_E	0x0BEE
#define PCI_DEVICE_ID_CDV_F	0x0BEF

/* ================================================================
 * Enumerations
 * ================================================================ */

enum gma3600_pipe {
	PIPE_A = 0,
	PIPE_B = 1,
};

enum gma3600_output_type {
	GMA3600_OUTPUT_UNUSED = 0,
	GMA3600_OUTPUT_ANALOG,		/* VGA / CRT */
	GMA3600_OUTPUT_LVDS,
	GMA3600_OUTPUT_HDMI,
};

enum gma3600_gmbus_pin {
	GMA3600_GMBUS_SSC    = 1,
	GMA3600_GMBUS_VGADDC = 2,
	GMA3600_GMBUS_PANEL  = 3,
	GMA3600_GMBUS_DPB    = 4,
	GMA3600_GMBUS_DPC    = 5,
	GMA3600_GMBUS_DPD    = 6,
};

/* ================================================================
 * DPLL clock structures
 * ================================================================ */

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

/* ================================================================
 * GTT (Graphics Translation Table)
 * ================================================================ */

struct gma3600_gtt {
	resource_size_t  gtt_phys;	   /* Physical addr of GTT           */
	void __iomem    *gtt_map;	   /* ioremapped GTT PTE array        */
	u32		 gtt_total_entries;/* Total PTE count                 */
	u32		 stolen_base;	   /* Physical base of stolen memory  */
	u32		 stolen_size;	   /* Bytes of stolen memory           */
	u32		 gtt_stolen_entries;/* PTEs pre-mapped to stolen mem  */
	unsigned long   *gtt_free_bitmap;/* 1 = free, indexed by PTE #     */
	struct mutex	 lock;
};

/* ================================================================
 * GEM buffer objects
 * ================================================================ */

struct gma3600_gem_object {
	struct drm_gem_object base;

	struct page	**pages;	/* Backing page array              */
	int		  num_pages;
	dma_addr_t	 *dma_addrs;	/* Per-page DMA addresses          */

	bool		  in_gtt;
	u32		  gtt_offset;	/* Byte offset inside GTT aperture */
	u32		  gtt_num_pages;

	bool		  stolen;	/* Allocated from stolen memory?   */
	u32		  stolen_offset;

	void		 *vaddr;	/* Kernel-mapped virtual address   */
};

static inline struct gma3600_gem_object *
to_gma3600_gem(struct drm_gem_object *obj)
{
	return container_of(obj, struct gma3600_gem_object, base);
}

/* ================================================================
 * GMBUS I²C controller
 * ================================================================ */

#define GMA3600_NUM_GMBUS_PORTS 7

struct gma3600_gmbus {
	struct i2c_adapter  adapter;
	u32		    pin;	/* GMBUS pin number          */
	struct gma3600_device *gdev;
};

/* ================================================================
 * Display: CRTC / Plane / Encoder / Connector
 * ================================================================ */

struct gma3600_crtc {
	struct drm_crtc		base;
	enum gma3600_pipe	pipe;
	struct gma3600_dpll_clock clock;
	bool			active;

	struct drm_plane	primary;
	struct drm_plane	cursor;
};

static inline struct gma3600_crtc *
to_gma3600_crtc(struct drm_crtc *crtc)
{
	return container_of(crtc, struct gma3600_crtc, base);
}

struct gma3600_encoder {
	struct drm_encoder	  base;
	enum gma3600_output_type  type;
	int			  crtc_mask;
	struct gma3600_device	 *gdev;
};

static inline struct gma3600_encoder *
to_gma3600_encoder(struct drm_encoder *enc)
{
	return container_of(enc, struct gma3600_encoder, base);
}

struct gma3600_connector {
	struct drm_connector	  base;
	enum gma3600_output_type  type;
	struct gma3600_encoder	 *encoder;
	struct gma3600_gmbus	 *ddc_bus;
};

static inline struct gma3600_connector *
to_gma3600_connector(struct drm_connector *c)
{
	return container_of(c, struct gma3600_connector, base);
}

/* ================================================================
 * LVDS panel data
 * ================================================================ */

struct gma3600_lvds {
	struct drm_display_mode *fixed_mode;
	bool   is_dual_link;
	u32    saved_lvds_reg;
	u32    saved_pp_control;
	u32    saved_pp_on_delays;
	u32    saved_pp_off_delays;
	u32    saved_pp_divisor;
	u32    saved_blc_pwm_ctl;
	u32    saved_blc_pwm_ctl2;
};

/* ================================================================
 * Backlight
 * ================================================================ */

struct gma3600_backlight {
	struct backlight_device *device;
	u32    max_brightness;
	u32    level;
	bool   enabled;
};

/* ================================================================
 * Top-level device
 * ================================================================ */

struct gma3600_device {
	struct drm_device drm;
	struct pci_dev	 *pdev;
	struct pci_dev	 *bridge;	/* Host bridge PCI 0:0.0        */

	/* MMIO (BAR 0) */
	void __iomem	 *mmio;
	resource_size_t	  mmio_phys;
	resource_size_t	  mmio_size;

	/* Aperture (BAR 2) */
	resource_size_t	  aperture_phys;
	resource_size_t	  aperture_size;

	/* GTT */
	struct gma3600_gtt gtt;

	/* GMBUS I²C */
	struct gma3600_gmbus gmbus[GMA3600_NUM_GMBUS_PORTS];

	/* CRTCs */
	struct gma3600_crtc crtcs[GMA3600_MAX_PIPES];

	/* LVDS */
	struct gma3600_lvds lvds;
	bool   has_lvds;

	/* Backlight */
	struct gma3600_backlight backlight;

	/* Interrupts */
	spinlock_t irq_lock;
	u32	   irq_mask;

	/* Power-management saved state */
	bool suspended;
	u32  saveDSPACNTR, saveDSPBCNTR;
	u32  savePIPEACONF, savePIPEBCONF;
	u32  saveDPLL_A,  saveDPLL_B;
	u32  saveFPA0, saveFPA1, saveFPB0, saveFPB1;
	u32  saveHTOTAL_A, saveHBLANK_A, saveHSYNC_A;
	u32  saveVTOTAL_A, saveVBLANK_A, saveVSYNC_A;
	u32  saveHTOTAL_B, saveHBLANK_B, saveHSYNC_B;
	u32  saveVTOTAL_B, saveVBLANK_B, saveVSYNC_B;
	u32  saveDSPASTRIDE, saveDSPBSTRIDE;
	u32  saveDSPASIZE,  saveDSPBSIZE;
	u32  saveDSPAPOS,   saveDSPBPOS;
	u32  saveDSPASURF,  saveDSPBSURF;
	u32  saveADPA, saveLVDS, saveHDMIB;
	u32  saveVGACNTRL;
	u32  savePIPEASRC, savePIPEBSRC;
	u32  saveBLC_PWM_CTL, saveBLC_PWM_CTL2;

	/* SSC */
	int  lvds_ssc_freq;	/* kHz (96 000 or 100 000) */
	bool lvds_use_ssc;

	/* Reference clock mode (true = 96 MHz, false = 27 MHz) */
	bool dplla_96mhz;
};

extern int gma3600_refclk;

static inline struct gma3600_device *
to_gma3600(struct drm_device *dev)
{
	return container_of(dev, struct gma3600_device, drm);
}

/* ================================================================
 * Register-access helpers
 * ================================================================ */

static inline u32 gma3600_read(struct gma3600_device *gdev, u32 reg)
{
	return ioread32(gdev->mmio + reg);
}

static inline void gma3600_write(struct gma3600_device *gdev, u32 reg,
				 u32 val)
{
	iowrite32(val, gdev->mmio + reg);
}

static inline void gma3600_write_post(struct gma3600_device *gdev,
				      u32 reg, u32 val)
{
	iowrite32(val, gdev->mmio + reg);
	(void)ioread32(gdev->mmio + reg);	/* posting read */
}

/* ================================================================
 * Per-file function declarations
 * ================================================================ */

/* gma3600_device.c */
int  gma3600_device_init(struct gma3600_device *gdev);
void gma3600_device_fini(struct gma3600_device *gdev);

/* gma3600_gtt.c */
int  gma3600_gtt_init(struct gma3600_device *gdev);
void gma3600_gtt_fini(struct gma3600_device *gdev);
int  gma3600_gtt_insert(struct gma3600_device *gdev,
			struct gma3600_gem_object *obj);
void gma3600_gtt_remove(struct gma3600_device *gdev,
			struct gma3600_gem_object *obj);
void gma3600_gtt_write_pte(struct gma3600_device *gdev,
			   u32 entry, dma_addr_t addr);
void gma3600_gtt_clear_pte(struct gma3600_device *gdev, u32 entry);

/* gma3600_gem.c */
int  gma3600_gem_init(struct gma3600_device *gdev);
struct gma3600_gem_object *
     gma3600_gem_create(struct gma3600_device *gdev, size_t size);
void gma3600_gem_free_object(struct drm_gem_object *obj);
int  gma3600_gem_dumb_create(struct drm_file *file, struct drm_device *dev,
			     struct drm_mode_create_dumb *args);
int  gma3600_gem_mmap_offset(struct drm_file *file, struct drm_device *dev,
			     u32 handle, u64 *offset);

/* gma3600_display.c */
int  gma3600_display_init(struct gma3600_device *gdev);
void gma3600_display_fini(struct gma3600_device *gdev);
void gma3600_update_wm(struct gma3600_device *gdev, enum gma3600_pipe pipe, bool is_lvds);

/* gma3600_dpll.c */
int  gma3600_sb_read(struct gma3600_device *gdev, u32 reg, u32 *val);
int  gma3600_sb_write(struct gma3600_device *gdev, u32 reg, u32 val);
void gma3600_sb_reset(struct gma3600_device *gdev);
bool gma3600_dpll_find_best(int refclk, int target,
			    const struct gma3600_dpll_limits *limits,
			    struct gma3600_dpll_clock *best);
int  gma3600_dpll_program(struct gma3600_device *gdev,
			  enum gma3600_pipe pipe,
			  const struct gma3600_dpll_clock *clock,
			  bool is_lvds,
			  bool is_hdmi);
void gma3600_dpll_enable(struct gma3600_device *gdev,
			 enum gma3600_pipe pipe);
void gma3600_dpll_disable(struct gma3600_device *gdev,
			  enum gma3600_pipe pipe);
const struct gma3600_dpll_limits *
     gma3600_dpll_get_limits(enum gma3600_output_type type, int refclk);

/* gma3600_lvds.c */
int  gma3600_lvds_init(struct gma3600_device *gdev);
bool gma3600_lvds_detect_panel(struct gma3600_device *gdev);

/* gma3600_crt.c */
int  gma3600_crt_init(struct gma3600_device *gdev);

/* gma3600_hdmi.c */
int  gma3600_hdmi_init(struct gma3600_device *gdev);

/* gma3600_i2c.c */
int  gma3600_i2c_init(struct gma3600_device *gdev);
void gma3600_i2c_fini(struct gma3600_device *gdev);
struct i2c_adapter *gma3600_i2c_get_adapter(struct gma3600_device *gdev,
					    enum gma3600_gmbus_pin pin);

/* gma3600_irq.c */
int  gma3600_irq_install(struct gma3600_device *gdev);
void gma3600_irq_uninstall(struct gma3600_device *gdev);
void gma3600_irq_enable_vblank(struct gma3600_device *gdev,
			       enum gma3600_pipe pipe);
void gma3600_irq_disable_vblank(struct gma3600_device *gdev,
				enum gma3600_pipe pipe);

/* gma3600_pm.c */
int gma3600_pm_suspend(struct device *dev);
int gma3600_pm_resume(struct device *dev);
int gma3600_pm_freeze(struct device *dev);
int gma3600_pm_thaw(struct device *dev);

/* gma3600_backlight.c */
int  gma3600_backlight_init(struct gma3600_device *gdev);
void gma3600_backlight_fini(struct gma3600_device *gdev);
void gma3600_backlight_set(struct gma3600_device *gdev, u32 level);
u32  gma3600_backlight_get(struct gma3600_device *gdev);

/* gma3600_fb.c */
int gma3600_fbdev_init(struct gma3600_device *gdev);
struct drm_framebuffer *
    gma3600_fb_create(struct drm_device *dev, struct drm_file *file,
		      const struct drm_mode_fb_cmd2 *cmd);

#endif /* __GMA3600_DRV_H__ */
