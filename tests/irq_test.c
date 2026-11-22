// SPDX-License-Identifier: GPL-2.0-only
/* Exercise the production IRQ path, including concurrent detach of a live ISR. */
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "irq_stubs.h"
#include "gma3600_reg.h"

static struct gma3600_device device;
static struct pci_dev pci = { .irq = 16 };
static struct gma3600_sgx_device sgx = { .host = &device };
static const struct gma3600_sgx_irq_ops *ops = &gma3600_sgx_irq_ops;
static irqreturn_t (*installed_handler)(int, void *);
static void *installed_data;
static unsigned int vblanks[2], sgx_calls;
static bool sgx_source, unhandled, block_callback;
static int cookie, other_cookie;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_cond = PTHREAD_COND_INITIALIZER;
static unsigned int active_irqs;
static bool callback_entered, release_callback, sync_entered;
static atomic_bool unregister_returned;

u32 gma3600_read(struct gma3600_device *gdev, u32 reg)
{
    return gdev->registers[reg / 4];
}

void gma3600_write(struct gma3600_device *gdev, u32 reg, u32 value)
{
    if (reg == IIR) {
        if (value & DE_PIPEA_EVENT)
            assert(!(gdev->registers[PIPEASTAT / 4] & PIPE_VBLANK_INT_STATUS));
        if (value & DE_PIPEB_EVENT)
            assert(!(gdev->registers[PIPEBSTAT / 4] & PIPE_VBLANK_INT_STATUS));
        /* An enabled SGX source must be cleared locally before top-level ack. */
        if ((value & GMA3600_SGX_IRQ_BIT) &&
            (gdev->irq_mask & GMA3600_SGX_IRQ_BIT))
            assert(!sgx_source);
        gdev->registers[reg / 4] &= ~value;
    } else if (reg == PIPEASTAT || reg == PIPEBSTAT) {
        gdev->registers[reg / 4] = (value & 0xffff0000U) |
            ((gdev->registers[reg / 4] & 0xffffU) & ~value);
    } else {
        gdev->registers[reg / 4] = value;
    }
}

void gma3600_write_post(struct gma3600_device *gdev, u32 reg, u32 value)
{ gma3600_write(gdev, reg, value); }
int pci_alloc_irq_vectors(struct pci_dev *p, int min, int max, int flags)
{ (void)p; assert(min == 1 && max == 1 && flags == PCI_IRQ_INTX); return 1; }
void pci_free_irq_vectors(struct pci_dev *p) { (void)p; }
int request_irq(int irq, irqreturn_t (*handler)(int, void *), int flags,
                const char *name, void *data)
{
    assert(irq == pci.irq && flags == IRQF_SHARED);
    assert(!strcmp(name, "gma3600"));
    installed_handler = handler;
    installed_data = data;
    return 0;
}
void free_irq(int irq, void *data)
{ assert(irq == pci.irq && data == installed_data); installed_handler = NULL; }
void drm_crtc_handle_vblank(struct drm_crtc *crtc)
{ vblanks[crtc->index]++; }

void synchronize_irq(int irq)
{
    assert(irq == pci.irq);
    assert(!pthread_mutex_lock(&state_lock));
    sync_entered = true;
    assert(!pthread_cond_broadcast(&state_cond));
    while (active_irqs)
        assert(!pthread_cond_wait(&state_cond, &state_lock));
    assert(!pthread_mutex_unlock(&state_lock));
}

static irqreturn_t gpu_isr(void *data)
{
    assert(data == &cookie);
    sgx_calls++;
    if (unhandled)
        return IRQ_NONE;
    /* A callback under irq_lock would deadlock a child taking that lock. */
    assert(!pthread_mutex_trylock(&device.irq_lock));
    assert(!pthread_mutex_unlock(&device.irq_lock));
    if (block_callback) {
        assert(!pthread_mutex_lock(&state_lock));
        callback_entered = true;
        assert(!pthread_cond_broadcast(&state_cond));
        while (!release_callback)
            assert(!pthread_cond_wait(&state_cond, &state_lock));
        assert(!pthread_mutex_unlock(&state_lock));
    }
    sgx_source = false;
    return IRQ_HANDLED;
}

static irqreturn_t dispatch(void)
{
    irqreturn_t ret;
    assert(!pthread_mutex_lock(&state_lock));
    active_irqs++;
    assert(!pthread_mutex_unlock(&state_lock));
    ret = installed_handler(pci.irq, installed_data);
    assert(!pthread_mutex_lock(&state_lock));
    active_irqs--;
    assert(!pthread_cond_broadcast(&state_cond));
    assert(!pthread_mutex_unlock(&state_lock));
    return ret;
}

static void *irq_thread(void *unused)
{ (void)unused; assert(dispatch() == IRQ_HANDLED); return NULL; }
static void *unregister_thread(void *unused)
{
    (void)unused;
    ops->unregister_handler(&sgx, &cookie);
    atomic_store(&unregister_returned, true);
    return NULL;
}

int main(void)
{
    pthread_t irq, unregister;
    device.pdev = &pci;
    device.crtcs[PIPE_B].base.index = PIPE_B;
    assert(!pthread_mutex_init(&device.irq_lock, NULL));
    gma3600_sgx_irq_init(&device);
    assert(!gma3600_irq_install(&device));

    assert(ops->enable(&sgx, &cookie, true) == -ENOENT);
    assert(ops->register_handler(&sgx, NULL, &cookie) == -EINVAL);
    assert(ops->register_handler(&sgx, gpu_isr, NULL) == -EINVAL);
    assert(!ops->register_handler(&sgx, gpu_isr, &cookie));
    assert(!(device.irq_mask & GMA3600_SGX_IRQ_BIT));
    assert(ops->register_handler(&sgx, gpu_isr, &other_cookie) == -EBUSY);
    assert(ops->enable(&sgx, &other_cookie, true) == -ENOENT);
    assert(!ops->enable(&sgx, &cookie, true));
    gma3600_irq_enable_vblank(&device, PIPE_A);
    gma3600_irq_enable_vblank(&device, PIPE_B);
    assert(device.irq_mask == (GMA3600_SGX_IRQ_BIT | DE_PIPEA_EVENT | DE_PIPEB_EVENT));
    assert(gma3600_read(&device, IER) == device.irq_mask);
    assert(gma3600_read(&device, IMR) == ~device.irq_mask);
    ops->unregister_handler(&sgx, &other_cookie);
    assert(device.sgx_irq_handler == gpu_isr);

    /* SGX and both displays in a single shared interrupt. */
    device.registers[IIR / 4] = device.irq_mask;
    device.registers[PIPEASTAT / 4] |= PIPE_VBLANK_INT_STATUS;
    device.registers[PIPEBSTAT / 4] |= PIPE_VBLANK_INT_STATUS;
    sgx_source = true;
    assert(dispatch() == IRQ_HANDLED);
    assert(sgx_calls == 1 && vblanks[0] == 1 && vblanks[1] == 1);
    assert(!device.registers[IIR / 4] && !sgx_source);

    assert(!ops->enable(&sgx, &cookie, false));
    assert(device.irq_mask == (DE_PIPEA_EVENT | DE_PIPEB_EVENT));
    assert(gma3600_read(&device, IMR) == ~device.irq_mask);
    device.registers[IIR / 4] = GMA3600_SGX_IRQ_BIT;
    assert(dispatch() == IRQ_NONE); /* Do not ack masked/unowned events. */
    assert(device.registers[IIR / 4] == GMA3600_SGX_IRQ_BIT);
    device.registers[IIR / 4] = 0;
    assert(!ops->enable(&sgx, &cookie, true));
    unhandled = true;
    device.registers[IIR / 4] = GMA3600_SGX_IRQ_BIT;
    sgx_source = true;
    assert(dispatch() == IRQ_HANDLED);
    assert(device.irq_mask == (DE_PIPEA_EVENT | DE_PIPEB_EVENT));
    assert(gma3600_read(&device, IER) == device.irq_mask);
    assert(sgx_source); /* An uncleared source is now masked, not storming. */
    assert(gma3600_read(&device, IMR) == ~device.irq_mask);
    unhandled = false;

    /* Detach must wait for a callback already running outside irq_lock. */
    assert(!ops->enable(&sgx, &cookie, true));
    block_callback = true;
    device.registers[IIR / 4] = GMA3600_SGX_IRQ_BIT;
    sgx_source = true;
    assert(!pthread_create(&irq, NULL, irq_thread, NULL));
    assert(!pthread_mutex_lock(&state_lock));
    while (!callback_entered)
        assert(!pthread_cond_wait(&state_cond, &state_lock));
    assert(!pthread_mutex_unlock(&state_lock));
    assert(!pthread_create(&unregister, NULL, unregister_thread, NULL));
    assert(!pthread_mutex_lock(&state_lock));
    while (!sync_entered)
        assert(!pthread_cond_wait(&state_cond, &state_lock));
    assert(!atomic_load(&unregister_returned));
    assert(device.irq_mask == (DE_PIPEA_EVENT | DE_PIPEB_EVENT));
    release_callback = true;
    assert(!pthread_cond_broadcast(&state_cond));
    assert(!pthread_mutex_unlock(&state_lock));
    assert(!pthread_join(irq, NULL));
    assert(!pthread_join(unregister, NULL));
    assert(atomic_load(&unregister_returned));
    assert(!device.sgx_irq_handler && !device.sgx_irq_data);
    assert(!ops->register_handler(&sgx, gpu_isr, &cookie));
    gma3600_sgx_irq_shutdown(&device);
    assert(ops->register_handler(&sgx, gpu_isr, &cookie) == -ENODEV);
    assert(ops->enable(&sgx, &cookie, true) == -ENODEV);
    assert(device.irq_mask == (DE_PIPEA_EVENT | DE_PIPEB_EVENT));
    gma3600_irq_uninstall(&device);
    puts("Shared IRQ tests passed: source/ack ordering, display mask preservation, ownership, live detach");
    return 0;
}
