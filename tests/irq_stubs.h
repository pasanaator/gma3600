/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA3600_IRQ_TEST_STUBS_H
#define GMA3600_IRQ_TEST_STUBS_H
#define __GMA3600_DRV_H__
#define GMA3600_SGX_H
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

typedef uint32_t u32;
typedef enum { IRQ_NONE, IRQ_HANDLED } irqreturn_t;
enum gma3600_pipe { PIPE_A, PIPE_B };
typedef pthread_mutex_t spinlock_t;
struct mutex { pthread_mutex_t lock; };
static inline void mutex_init(struct mutex *m)
{ assert(!pthread_mutex_init(&m->lock, NULL)); }
static inline void mutex_lock(struct mutex *m)
{ assert(!pthread_mutex_lock(&m->lock)); }
static inline void mutex_unlock(struct mutex *m)
{ assert(!pthread_mutex_unlock(&m->lock)); }
#define spin_lock_irqsave(lock, flags) do { \
    (flags) = 0; assert(!pthread_mutex_lock(lock)); \
} while (0)
#define spin_unlock_irqrestore(lock, flags) do { \
    (void)(flags); assert(!pthread_mutex_unlock(lock)); \
} while (0)
#define GMA3600_SGX_IRQ_BIT (1U << 18)
#define IRQF_SHARED 1
#define PCI_IRQ_INTX 1
#define DRIVER_NAME "gma3600"
#define dev_err(...) ((void)0)
#define dev_warn_ratelimited(...) ((void)0)
struct device { int pm_refs; bool pm_enabled, pm_active; };
struct pci_dev { int irq, current_state, refs; bool offline; unsigned int vendor, device; struct device dev; };
static inline int pm_runtime_get_if_active(struct device *d)
{ if (!d->pm_enabled) return -EINVAL; if (!d->pm_active) return 0; d->pm_refs++; return 1; }
static inline bool pm_runtime_enabled(struct device *d) { return d->pm_enabled; }
static inline void pm_runtime_get_noresume(struct device *d) { d->pm_refs++; }
static inline void pm_runtime_put_noidle(struct device *d) { assert(d->pm_refs > 0); d->pm_refs--; }
struct drm_device { int refs; };
#define PCI_D0 0
static inline bool pci_channel_offline(struct pci_dev *p) { return p->offline; }
static inline void drm_dev_get(struct drm_device *d) { d->refs++; }
static inline void drm_dev_put(struct drm_device *d) { assert(d->refs > 0); d->refs--; }
static inline void pci_dev_get(struct pci_dev *p) { p->refs++; }
static inline void pci_dev_put(struct pci_dev *p) { assert(p->refs > 0); p->refs--; }
#define THIS_MODULE NULL
struct drm_crtc { int index; };
struct gma3600_device {
    struct drm_device drm;
    struct pci_dev *pdev;
    spinlock_t irq_lock;
    u32 irq_mask;
    struct mutex sgx_irq_mutex;
    struct mutex sgx_lifecycle_mutex;
    bool sgx_suspending, sgx_removing, suspended;
    bool sgx_lease_sealed;
    void *sgx_lease_owner;
    bool sgx_irq_closing;
    irqreturn_t (*sgx_irq_handler)(void *);
    void *sgx_irq_data;
    struct { struct drm_crtc base; } crtcs[2];
    u32 registers[0x80000 / 4];
};
struct gma3600_sgx_device { struct gma3600_device *host; struct pci_dev *pdev; };
struct gma3600_sgx_lease_ops {
    void *owner;
    int (*acquire)(struct gma3600_sgx_device *, void *);
    int (*release)(struct gma3600_sgx_device *, void *);
    int (*seal)(struct gma3600_sgx_device *, void *);
};
extern const struct gma3600_sgx_lease_ops gma3600_sgx_lease_ops;
void gma3600_sgx_lifecycle_init(struct gma3600_device *);
int gma3600_sgx_suspend_begin(struct gma3600_device *);
void gma3600_sgx_suspend_end(struct gma3600_device *);
void gma3600_sgx_remove_begin(struct gma3600_device *);
struct gma3600_sgx_irq_ops {
    int (*register_handler)(struct gma3600_sgx_device *, irqreturn_t (*)(void *), void *);
    int (*enable)(struct gma3600_sgx_device *, void *, bool);
    void (*unregister_handler)(struct gma3600_sgx_device *, void *);
};
extern const struct gma3600_sgx_irq_ops gma3600_sgx_irq_ops;
void gma3600_sgx_irq_init(struct gma3600_device *gdev);
void gma3600_sgx_irq_shutdown(struct gma3600_device *gdev);
void gma3600_sgx_irq_dispatch(struct gma3600_device *gdev, u32 iir);
void gma3600_irq_enable_vblank(struct gma3600_device *, enum gma3600_pipe);
int gma3600_irq_install(struct gma3600_device *);
void gma3600_irq_uninstall(struct gma3600_device *);
u32 gma3600_read(struct gma3600_device *, u32);
void gma3600_write(struct gma3600_device *, u32, u32);
void gma3600_write_post(struct gma3600_device *, u32, u32);
int pci_alloc_irq_vectors(struct pci_dev *, int, int, int);
void pci_free_irq_vectors(struct pci_dev *);
int request_irq(int, irqreturn_t (*)(int, void *), int, const char *, void *);
void free_irq(int, void *);
void synchronize_irq(int);
void drm_crtc_handle_vblank(struct drm_crtc *);
#endif
