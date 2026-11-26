// SPDX-License-Identifier: GPL-2.0-only
#include "irq_stubs.h"
#include <stdio.h>
static struct gma3600_device device;
static struct pci_dev pci = {.refs=1,.vendor=0x8086,.device=0x0be1};
static struct gma3600_sgx_device sgx = {.host=&device, .pdev=&pci};
static int owner, other, lease_result, suspend_result;
static pthread_barrier_t barrier;
static void *acquire_thread(void *unused)
{
    (void)unused; pthread_barrier_wait(&barrier);
    lease_result = gma3600_sgx_lease_ops.acquire(&sgx, &owner); return NULL;
}
static void *suspend_thread(void *unused)
{
    (void)unused; pthread_barrier_wait(&barrier);
    suspend_result = gma3600_sgx_suspend_begin(&device); return NULL;
}
int main(void)
{
    unsigned i;
    gma3600_sgx_lifecycle_init(&device); mutex_init(&device.sgx_irq_mutex);
    device.drm.refs = 1;
    assert(gma3600_sgx_lease_ops.acquire(&sgx, NULL) == -EINVAL);
    assert(!gma3600_sgx_lease_ops.acquire(&sgx, &owner));
    assert(device.drm.refs == 2 && pci.refs == 2);
    assert(pci.dev.pm_refs == 1);
    assert(gma3600_sgx_suspend_begin(&device) == -EBUSY);
    assert(gma3600_sgx_lease_ops.acquire(&sgx, &other) == -EBUSY);
    assert(gma3600_sgx_lease_ops.release(&sgx, &other) == -ENOENT);
    assert(!gma3600_sgx_lease_ops.release(&sgx, &owner));
    assert(gma3600_sgx_lease_ops.release(&sgx, &owner) == -ENOENT);
    assert(device.drm.refs == 1 && pci.refs == 1);
    assert(!pci.dev.pm_refs);
    pci.dev.pm_enabled = true;
    assert(gma3600_sgx_lease_ops.acquire(&sgx, &owner) == -EAGAIN && !pci.dev.pm_refs);
    pci.dev.pm_active = true;
    assert(!gma3600_sgx_lease_ops.acquire(&sgx, &owner) && pci.dev.pm_refs == 1);
    assert(!gma3600_sgx_lease_ops.release(&sgx, &owner) && !pci.dev.pm_refs);
    pci.dev.pm_enabled = false;
    assert(!gma3600_sgx_suspend_begin(&device));
    assert(gma3600_sgx_lease_ops.acquire(&sgx, &owner) == -EBUSY);
    /* Failed display suspend / completed resume clears the transition. */
    gma3600_sgx_suspend_end(&device);
    pci.current_state = 3;
    assert(gma3600_sgx_lease_ops.acquire(&sgx, &owner) == -EAGAIN);
    pci.current_state = 0; pci.offline = true;
    assert(gma3600_sgx_lease_ops.acquire(&sgx, &owner) == -EAGAIN);
    pci.offline = false; device.suspended = true;
    assert(gma3600_sgx_lease_ops.acquire(&sgx, &owner) == -EBUSY);
    device.suspended = false;
    for (i = 0; i < 32; i++) {
        pthread_t a,b;
        assert(!pthread_barrier_init(&barrier, NULL, 2));
        assert(!pthread_create(&a, NULL, acquire_thread, NULL));
        assert(!pthread_create(&b, NULL, suspend_thread, NULL));
        pthread_join(a, NULL); pthread_join(b, NULL);
        assert((lease_result == 0) != (suspend_result == 0));
        if (!lease_result) assert(!gma3600_sgx_lease_ops.release(&sgx, &owner));
        else gma3600_sgx_suspend_end(&device);
        assert(device.drm.refs == 1 && pci.refs == 1);
        pthread_barrier_destroy(&barrier);
    }
    assert(!gma3600_sgx_lease_ops.acquire(&sgx, &owner));
    gma3600_sgx_remove_begin(&device);
    assert(gma3600_sgx_suspend_begin(&device) == -ENODEV);
    /* Forced unbind excludes new leases but must still let the old child
     * release its CPU-only refs before parent storage is finally destroyed. */
    assert(!gma3600_sgx_lease_ops.release(&sgx, &owner));
    assert(gma3600_sgx_lease_ops.acquire(&sgx, &owner) == -ENODEV);
    assert(device.drm.refs == 1 && pci.refs == 1);
    /* A separate owner models the first live boot's irreversible retention.
     * This changes only software state; no register writes occur in seal. */
    device.sgx_removing=false;
    assert(!gma3600_sgx_lease_ops.acquire(&sgx,&owner));
    assert(gma3600_sgx_lease_ops.seal(&sgx,&other)==-ENOENT);
    pci.device=0x0be0;
    assert(gma3600_sgx_lease_ops.seal(&sgx,&owner)==-ENODEV);
    pci.device=0x0be1;
    assert(!gma3600_sgx_lease_ops.seal(&sgx,&owner));
    assert(gma3600_sgx_lease_ops.seal(&sgx,&owner)==-EALREADY);
    assert(gma3600_sgx_lease_ops.release(&sgx,&owner)==-EBUSY);
    gma3600_sgx_remove_begin(&device);
    assert(gma3600_sgx_lease_ops.release(&sgx,&owner)==-EBUSY);
    assert(device.drm.refs==2 && pci.refs==2 && pci.dev.pm_refs==1 && device.sgx_lease_owner==&owner);
    puts("SGX parent lease tests passed: device refs, PM exclusion, acquire/suspend races, ownership and remove");
    return 0;
}
