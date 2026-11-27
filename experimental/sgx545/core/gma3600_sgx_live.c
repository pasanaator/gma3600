// SPDX-License-Identifier: GPL-2.0-only
/* Explicit one-shot, polling-only provider. No generic native-MMIO unlock. */
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/string.h>
#include "gma3600_sgx_live.h"
#include "gma3600_sgx_live_policy.h"
#include "gma3600_sgx_bridge.h"

/* The bridge and global DDK mutex serialize this scope. current additionally
 * excludes unrelated callers: even identification/recovery paths stay blocked. */
static struct gma_sgx_live *native_scope;

static int available(struct gma_sgx_live *l)
{
	void *base = NULL;
	if (!l->attached || gma3600_sgx_owner_hardware(l->identity, &base) || base != l->registers)
		return -ENODEV;
	return 0;
}
static void write_raw(struct gma_sgx_live *l, __u32 offset, __u32 value)
{
	if (l->io_error == -ENODEV) return;
	/* Flush WC page-directory/PTE/firmware writes before device visibility.
	 * A read from the documented reset register drains posted PCI writes. */
	wmb();
	writel(value, (void __iomem *)l->registers + offset);
	if (readl((void __iomem *)l->registers + GMA_LIVE_RESET) == ~0U) l->io_error = -ENODEV;
}
static int scoped(struct gma_sgx_live *l)
{
	return l && l->sealed && l->attached && READ_ONCE(l->task) == current && !in_interrupt();
}
static int scope_error(struct gma_sgx_live *l)
{
	if (!l->io_error && (++l->native_accesses > 4096 ||
	    ktime_get_ns()/1000 - l->boot.started_us >= GMA_SGX_BOOT_DEADLINE_US)) l->io_error = -ETIMEDOUT;
	if (!l->io_error) l->io_error = available(l);
	return l->io_error;
}
int gma3600_sgx_live_read(void *base, unsigned int offset, unsigned int *value)
{
	struct gma_sgx_live *l = READ_ONCE(native_scope);
	if (!scoped(l)) return 0;
	*value = 0;
	if (base != l->registers || !gma_sgx_reset_read_allowed(offset)) l->io_error = -EACCES;
	if (!scope_error(l)) {
		*value = readl((void __iomem *)base + offset);
		if (*value == ~0U && (offset == GMA_LIVE_BIF_STATUS || offset == GMA_LIVE_RESET))
			l->io_error = -ENODEV;
	}
	return 1; /* Consume and suppress denied native accesses too. */
}
int gma3600_sgx_live_write(void *base, unsigned int offset, unsigned int value)
{
	struct gma_sgx_live *l = READ_ONCE(native_scope);
	if (!scoped(l)) return 0;
	if (base != l->registers || !gma_sgx_reset_write_allowed(&l->roots, offset, value)) l->io_error = -EACCES;
	if (!scope_error(l)) write_raw(l, offset, value);
	return 1;
}
static int acquire(void *context, const struct gma_sgx_boot_plan *plan)
{
	struct gma_sgx_live *l = context;
	(void)plan;
	return l->owner_ops->acquire(l->owner);
}
static int verify(void *context, const struct gma_sgx_boot_plan *plan)
{
	struct gma_sgx_live *l = context;
	int ret = l->owner_ops->verify(l->owner);
	(void)plan;
	if (!ret) ret = gma3600_sgx_owner_hardware(l->identity, &l->registers);
	if (!ret) ret = gma3600_sgx_native_live_prepare(l->runtime, l->registers, &l->roots);
	return ret;
}
static int write_reg(void *context, __u32 offset, __u32 value)
{
	struct gma_sgx_live *l = context;
	int ret = available(l);
	if (ret) return ret;
	if (!l->sealed) {
		ret = gma3600_sgx_owner_seal(l->identity);
		if (ret) return ret;
		l->sealed = 1; /* Irreversible, including failures before first writel. */
		ret = gma3600_sgx_native_live_arm(l->runtime);
		if (ret) return ret;
	}
	if ((offset & 3) || offset >= 0x4000 || ((offset == 0x130 || offset == 0x110) && value)) return -EACCES;
	write_raw(l, offset, value); /* Only the validated controller plan calls this. */
	return l->io_error;
}
static int reset(void *context)
{
	struct gma_sgx_live *l = context;
	int ret;
	if (!l->sealed || native_scope) return -EBUSY;
	WRITE_ONCE(l->task, current); WRITE_ONCE(native_scope, l);
	ret = gma3600_sgx_native_live_reset(l->runtime, &l->reset_error);
	WRITE_ONCE(native_scope, NULL); WRITE_ONCE(l->task, NULL);
	return l->io_error ? l->io_error : ret;
}
static int publish(void *context)
{
	struct gma_sgx_live *l = context;
	return gma3600_sgx_native_live_publish(l->runtime);
}
static int status(void *context, __u32 *value)
{
	struct gma_sgx_live *l = context;
	int ret = available(l);
	if (ret) return ret;
	l->bif_status = readl((void __iomem *)l->registers + GMA_LIVE_BIF_STATUS);
	if (l->bif_status == ~0U) return l->io_error = -ENODEV;
	if (l->bif_status & 0xffffU) return -EIO;
	return gma3600_sgx_native_live_status(l->runtime, value);
}
static __u64 now(void *context) { (void)context; return ktime_get_ns()/1000; }
static void delay(void *context, __u32 usec) { (void)context; usleep_range(usec, usec+20); }
static void snapshot(struct gma_sgx_live *l)
{
	if (!l->sealed || l->io_error == -ENODEV || available(l)) return;
	l->bif_status = readl((void __iomem *)l->registers + GMA_LIVE_BIF_STATUS);
	if (l->bif_status == ~0U) { l->io_error = -ENODEV; return; }
	l->bif_fault = readl((void __iomem *)l->registers + GMA_LIVE_BIF_FAULT);
	l->event_status = readl((void __iomem *)l->registers + GMA_LIVE_EVENT_STATUS);
	l->event_status2 = readl((void __iomem *)l->registers + GMA_LIVE_EVENT_STATUS2);
}
static int contain(void *context)
{
	struct gma_sgx_live *l = context;
	if (!l->sealed) return 0;
	if (available(l) || l->io_error == -ENODEV) return -ENODEV;
	snapshot(l); /* Preserve diagnostics before best-effort reset assertion. */
	if (l->io_error == -ENODEV) return -ENODEV;
	write_raw(l, 0x130, 0); write_raw(l, 0x110, 0);
	write_raw(l, GMA_LIVE_RESET, GMA_LIVE_RESET_MASK);
	return 0; /* Containment is never a quiescence proof. */
}
static int quiesce(void *context)
{
	struct gma_sgx_live *l = context;
	return l->sealed ? -EOPNOTSUPP : 0;
}
static int release(void *context)
{
	struct gma_sgx_live *l = context;
	return l->owner_ops->release(l->owner);
}
static const struct gma_sgx_boot_ops boot_ops = {
	.acquire=acquire, .verify=verify, .write=write_reg, .reset=reset, .publish=publish,
	.status=status, .now_us=now, .delay_us=delay, .contain=contain, .quiesce=quiesce, .release=release,
};
int gma_sgx_live_start(struct gma_sgx_live *l, const struct sgx545_init2_info32 *info,
	const struct gma_sgx_boot_memory *memory, unsigned int count,
	const struct gma_sgx_live_owner_ops *ops, void *owner, void *identity, void *runtime)
{
	int ret;
	if (!l || !ops || !ops->acquire || !ops->verify || !ops->release || !runtime) return -EINVAL;
	l->owner_ops=ops; l->owner=owner; l->identity=identity; l->runtime=runtime; l->attached=1;
	ret = gma_sgx_boot_start(&l->boot, info, memory, count, &boot_ops, l);
	if (!ret) {
		snapshot(l);
		if (l->io_error || (l->bif_status & 0xffffU)) {
			l->boot.error = ret = l->io_error ? l->io_error : -EIO;
			gma_sgx_boot_stop(&l->boot);
		}
	}
	return ret;
}
void gma_sgx_live_result(struct gma_sgx_live *l, struct sgx545_live_boot *out)
{
	out->state=l->boot.state; out->sealed=l->sealed; out->init_status=l->boot.status;
	out->writes=l->boot.writes; out->polls=l->boot.polls; out->error=l->boot.error < 0 ? -l->boot.error : 0;
	out->reset_error=l->reset_error; out->bif_status=l->bif_status; out->bif_fault=l->bif_fault;
	out->event_status=l->event_status; out->event_status2=l->event_status2;
}
void gma_sgx_live_detach(struct gma_sgx_live *l)
{
	l->attached=0;
	gma_sgx_boot_detach(&l->boot); /* No MMIO and no freeing of a sealed owner. */
}
