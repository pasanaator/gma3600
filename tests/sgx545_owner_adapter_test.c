// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
typedef uint32_t u32;
typedef int PVRSRV_ERROR;
#define PVRSRV_OK 0
struct module { int refs; bool deny; };
static struct module child = {1,false}, parent = {1,false};
#define THIS_MODULE (&child)
static bool try_module_get(struct module *m) { if (m->deny) return false; m->refs++; return true; }
static void module_put(struct module *m) { assert(m->refs > 1); m->refs--; }
struct device { int refs; };
static void get_device(struct device *d) { d->refs++; }
static void put_device(struct device *d) { assert(d->refs > 1); d->refs--; }
static void dev_set_drvdata(struct device *d, void *data) { assert(d && !data); }
static int prepare_lock;
static void mutex_lock(int *lock) { assert(!*lock); *lock=1; }
static void mutex_unlock(int *lock) { assert(*lock); *lock=0; }
struct gma3600_sgx_device;
struct gma3600_sgx_lease_ops {
    struct module *owner;
    int (*acquire)(struct gma3600_sgx_device *, void *);
    int (*release)(struct gma3600_sgx_device *, void *);
    int (*seal)(struct gma3600_sgx_device *, void *);
};
struct gma3600_sgx_device { u32 host_abi; const struct gma3600_sgx_lease_ops *lease_ops; };
static void *held;
static int fail_acquire, fail_release, teardown, sealed;
static int acquire(struct gma3600_sgx_device *s, void *owner)
{ assert(s && !held && parent.refs==2 && child.refs==2); if(fail_acquire)return -EBUSY; held=owner;return 0; }
static int release(struct gma3600_sgx_device *s, void *owner)
{ assert(s && held==owner && parent.refs==2 && child.refs==2); if(fail_release)return -EIO; if(sealed)return -EBUSY; held=NULL;return 0; }
static int seal(struct gma3600_sgx_device *s,void *owner) { assert(s && held==owner);sealed=1;return 0; }
static const struct gma3600_sgx_lease_ops ops={&parent,acquire,release,seal};
static void *gpsPVRLDMDev;
static int SysDeinitialise(void *unused) { (void)unused; teardown++;return 0; }
#define SysAcquireDataNoCheck() NULL
#define LinuxMMCleanup() ((void)0)
#define PVROSFuncDeInit() ((void)0)
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
#define kfree free
#include "owner_functions.inc"
int main(void)
{
    struct device dev={1}; struct gma3600_sgx_device sgx={3,&ops};
    struct gma_sgx_prepared *ctx=calloc(1,sizeof(*ctx));
    u32 abi=0; int identity, wrong;
    assert(ctx);ctx->ready=true;ctx->dev=&dev;ctx->sgx=&sgx;active=ctx;
    /* ASan redzone models the old ABI object with no lease_ops tail. */
    {
        void *old_storage=calloc(1,offsetof(struct gma3600_sgx_device,lease_ops));
        struct gma3600_sgx_device *old=old_storage;
        assert(old);old->host_abi=2;ctx->sgx=old;
        assert(gma3600_sgx_owner_acquire(&identity,&abi)==-EOPNOTSUPP);
        assert(!abi && parent.refs==1 && child.refs==1);free(old);ctx->sgx=&sgx;
    }
    parent.deny=true;assert(gma3600_sgx_owner_acquire(&identity,&abi)==-ENODEV);parent.deny=false;
    child.deny=true;assert(gma3600_sgx_owner_acquire(&identity,&abi)==-ENODEV);child.deny=false;
    fail_acquire=1;assert(gma3600_sgx_owner_acquire(&identity,&abi)==-EBUSY);fail_acquire=0;
    assert(parent.refs==1 && child.refs==1 && dev.refs==1);
    assert(!gma3600_sgx_owner_acquire(&identity,&abi) && abi==3 && held==&identity);
    assert(parent.refs==2 && child.refs==2 && dev.refs==2);
    assert(gma3600_sgx_owner_release(&wrong)==-ENOENT);
    assert(gma3600_sgx_owner_acquire(&wrong,&abi)==-EBUSY);
    fail_release=1;assert(gma3600_sgx_owner_release(&identity)==-EIO);
    ctx->sys_ready=true;
    gma_sgx_prepare_cleanup(ctx);
    assert(active==ctx && !teardown && held && parent.refs==2 && child.refs==2 && dev.refs==2);
    fail_release=0;assert(!gma3600_sgx_owner_release(&identity));
    assert(parent.refs==1 && child.refs==1 && dev.refs==1 && !held);
    ctx->ready=true;
    assert(!gma3600_sgx_owner_acquire(&identity,&abi));
    assert(gma3600_sgx_owner_seal(&wrong)==-ENOENT);
    assert(!gma3600_sgx_owner_seal(&identity));
    assert(gma3600_sgx_owner_release(&identity)==-EBUSY);
    gma_sgx_prepare_cleanup(ctx);
    assert(active==ctx && !teardown && parent.refs==2 && child.refs==2 && dev.refs==2);
    /* Test seam only: production has no unseal API. */
    sealed=0;assert(!gma3600_sgx_owner_release(&identity));
    gma_sgx_prepare_cleanup(ctx);
    assert(!active && teardown==1);
    puts("SGX adapter owner tests passed: ABI2 tail safety, module/device pins, acquisition rollback and failed-lease common-teardown retention");
    return 0;
}
