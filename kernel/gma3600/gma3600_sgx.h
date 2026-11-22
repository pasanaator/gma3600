/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA3600_SGX_H
#define GMA3600_SGX_H

struct gma3600_device;

#ifdef GMA3600_SGX_HOST
#include <linux/auxiliary_bus.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/pci.h>

/* Cedarview sysconfig.h and SGX545 sgx545defs.h, not generic SGX offsets. */
#define GMA3600_SGX_REG_OFFSET 0x80000
#define GMA3600_SGX_REG_SIZE   0x4000
#define GMA3600_SGX_CORE_ID    0x001c
#define GMA3600_SGX_CORE_REV   0x0020
#define GMA3600_SGX_HOST_ABI   2
#define GMA3600_SGX_IRQ_BIT    (1U << 18)

struct gma3600_sgx_device;

/* Process-context calls; never invoke these from the registered ISR.
 * Registration leaves the source masked. Enable only after SGX is ready.
 * ISR runs in hard IRQ context, without the parent's irq_lock held. It must
 * clear its SGX-local event source before returning IRQ_HANDLED. The parent
 * alone acknowledges top-level IIR and changes IER/IMR under irq_lock.
 * Unregister disables the source and drains the parent IRQ before returning;
 * the child must stop producers, then unregister, then cancel deferred work
 * and free callback data. Unregister does not drain work queued by the ISR.
 */
struct gma3600_sgx_irq_ops {
	int (*register_handler)(struct gma3600_sgx_device *sgx,
				irqreturn_t (*handler)(void *), void *data);
	int (*enable)(struct gma3600_sgx_device *sgx, void *data, bool enable);
	void (*unregister_handler)(struct gma3600_sgx_device *sgx, void *data);
};

/* Private kernel interface, not a userspace ABI. Parent owns PCI/BAR/IRQ.
 * Child must not enable/disable PCI, request BAR0, change the display GTT,
 * reset the SoC, or install its own top-level interrupt handler.
 * Identification reads are permitted while in D0. Interrupt access must use
 * irq_ops. This interface does not authorize GPU power/reset/MMU writes.
 */
struct gma3600_sgx_device {
	struct auxiliary_device adev;
	struct pci_dev *pdev;
	void __iomem *regs;
	resource_size_t regs_phys;
	u32 host_abi;
	struct gma3600_device *host;
	const struct gma3600_sgx_irq_ops *irq_ops;
};

extern const struct gma3600_sgx_irq_ops gma3600_sgx_irq_ops;
void gma3600_sgx_irq_init(struct gma3600_device *gdev);
void gma3600_sgx_irq_shutdown(struct gma3600_device *gdev);
void gma3600_sgx_irq_dispatch(struct gma3600_device *gdev, u32 iir);
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
