/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA3600_SGX_H
#define GMA3600_SGX_H

struct gma3600_device;

#ifdef GMA3600_SGX_HOST
#include <linux/auxiliary_bus.h>
#include <linux/io.h>
#include <linux/pci.h>

/* Cedarview sysconfig.h and SGX545 sgx545defs.h, not generic SGX offsets. */
#define GMA3600_SGX_REG_OFFSET 0x80000
#define GMA3600_SGX_REG_SIZE   0x4000
#define GMA3600_SGX_CORE_ID    0x001c
#define GMA3600_SGX_CORE_REV   0x0020
#define GMA3600_SGX_HOST_ABI   1

/* Private kernel interface, not a userspace ABI. Parent owns PCI/BAR/IRQ.
 * Child must not enable/disable PCI, request BAR0, change the display GTT,
 * reset the SoC, or install its own top-level interrupt handler.
 * This first interface permits identification reads only, while in D0.
 */
struct gma3600_sgx_device {
	struct auxiliary_device adev;
	struct pci_dev *pdev;
	void __iomem *regs;
	resource_size_t regs_phys;
	u32 host_abi;
};

int gma3600_sgx_host_init(struct gma3600_device *gdev);
void gma3600_sgx_host_fini(struct gma3600_device *gdev);
#else
static inline int gma3600_sgx_host_init(struct gma3600_device *gdev)
{
	(void)gdev;
	return 0;
}
static inline void gma3600_sgx_host_fini(struct gma3600_device *gdev)
{
	(void)gdev;
}
#endif
#endif
