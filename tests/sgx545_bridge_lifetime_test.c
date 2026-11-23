// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sgx545_abi.h"

typedef uint32_t u32;
typedef void *IMG_HANDLE;
typedef int PVRSRV_HANDLE_BASE;
struct gma_sgx_bridge_stats { u32 files, connections, errors; };
struct list_head { struct list_head *next, *prev; };
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
#define INIT_LIST_HEAD(h) ((h)->next = (h)->prev = (h))
static bool list_empty(struct list_head *h) { return h->next == h; }
static void list_add(struct list_head *n, struct list_head *h)
{ n->next = h->next; n->prev = h; h->next->prev = n; h->next = n; }
static void list_del(struct list_head *n)
{ n->prev->next = n->next; n->next->prev = n->prev; }
#define list_for_each_entry_safe(pos, next_pos, head, member) \
    for (struct list_head *it = (head)->next, *nt = it->next; \
         it != (head) && ((pos) = container_of(it, __typeof__(*(pos)), member), \
         (next_pos) = nt == (head) ? NULL : container_of(nt, __typeof__(*(pos)), member), \
         (void)(next_pos), 1); it = nt, nt = it->next)
struct mutex { int locked; };
static void mutex_init(struct mutex *m) { m->locked = 0; }
static void mutex_lock(struct mutex *m) { assert(!m->locked); m->locked = 1; }
static void mutex_unlock(struct mutex *m) { assert(m->locked); m->locked = 0; }
static struct mutex gPVRSRVLock;
#define PVRSRV_LOCK_CLASS_BRIDGE 1
static void LinuxLockMutexNested(struct mutex *m, int cls) { mutex_lock(m); }
static void LinuxUnLockMutex(struct mutex *m) { mutex_unlock(m); }
struct kref { unsigned refs; };
static void kref_init(struct kref *r) { r->refs = 1; }
static void kref_get(struct kref *r) { assert(r->refs); r->refs++; }
static void kref_put(struct kref *r, void (*free_)(struct kref *))
{ assert(r->refs); if (!--r->refs) free_(r); }
struct pid { unsigned refs; };
static struct pid current_pid;
#define current (&current_pid)
#define task_tgid(task) (task)
static struct pid *get_pid(struct pid *p) { p->refs++; return p; }
static void put_pid(struct pid *p) { assert(p->refs); p->refs--; }
struct inode { int unused; };
struct file { void *private_data; };
struct device { void (*action)(void *); void *data; };
struct miscdevice {
    int minor, mode;
    const char *name;
    struct device *parent;
    const void *fops;
};
static int bridge_fops, allocations, fail_alloc, fail_misc, fail_devm;
static int fail_handle, fail_free, freed_bases, misc_live, ddk_alive = 1;
static void *watched_bridge;
#define GFP_KERNEL 0
#define U32_MAX UINT32_MAX
#define MISC_DYNAMIC_MINOR 255
#define WARN_ON(value) assert(!(value))
#define pr_info(...) ((void)0)
#define pr_err(...) ((void)0)
static void *kzalloc(size_t size, int flags)
{
    void *p;
    if (fail_alloc) { fail_alloc = 0; return NULL; }
    p = calloc(1, size);
    assert(p);
    allocations++;
    return p;
}
static void kfree(void *p);
static int nonseekable_open(struct inode *inode, struct file *file) { return 0; }
static int misc_register(struct miscdevice *m)
{ if (fail_misc) return -ENOMEM; assert(!misc_live); misc_live = 1; return 0; }
static void misc_deregister(struct miscdevice *m) { assert(misc_live); misc_live = 0; }
static int devm_add_action_or_reset(struct device *d, void (*action)(void *), void *p)
{
    if (fail_devm) { action(p); return -ENOMEM; }
    d->action = action; d->data = p;
    return 0;
}
#define PVRSRV_HANDLE_TYPE_PERPROC_DATA 1
#define PVRSRV_HANDLE_ALLOC_FLAG_NONE 0
static int PVRSRVAllocHandleBase(PVRSRV_HANDLE_BASE **base)
{ assert(ddk_alive); *base = kzalloc(sizeof(**base), 0); return *base ? 0 : 12; }
static int PVRSRVAllocHandle(PVRSRV_HANDLE_BASE *base, IMG_HANDLE *handle,
                            void *data, int type, int flags)
{ assert(ddk_alive && base); if (fail_handle) return 17; *handle = data; return 0; }
static int PVRSRVFreeHandleBase(PVRSRV_HANDLE_BASE *base)
{
    assert(ddk_alive && base);
    if (fail_free) { fail_free--; return 19; }
    freed_bases++; kfree(base); return 0;
}

#include "bridge_functions.inc"

static void kfree(void *p)
{
    assert(p && allocations > 0);
    if (p == watched_bridge) {
        assert(!((struct gma_sgx_bridge *)p)->lock.locked);
        watched_bridge = NULL;
    }
    allocations--; free(p);
}
static struct gma_sgx_bridge *start(struct device *dev)
{
    struct gma_sgx_bridge *b = NULL;
    assert(!gma3600_sgx_bridge_register(dev, &b) && b);
    watched_bridge = b;
    assert(b->misc.mode == 0600 && misc_live);
    return b;
}
static struct gma_sgx_file *open_(struct gma_sgx_bridge *b, struct file *file)
{
    file->private_data = &b->misc;
    assert(!bridge_open(NULL, file));
    return file->private_data;
}
static void stop(struct device *dev)
{ dev->action(dev->data); dev->action = NULL; dev->data = NULL; }
static void empty(void)
{ assert(!allocations && !current_pid.refs && !misc_live && !watched_bridge); }

int main(void)
{
    struct device dev = {0};
    struct gma_sgx_bridge *b, *result = NULL;
    struct file file, second;
    struct gma_sgx_file *f;
    u32 token;

    fail_alloc = 1;
    assert(gma3600_sgx_bridge_register(&dev, &result) == -ENOMEM && !result);
    empty();
    fail_devm = 1;
    assert(gma3600_sgx_bridge_register(&dev, &result) == -ENOMEM && !result);
    fail_devm = 0; empty();
    fail_misc = 1;
    assert(gma3600_sgx_bridge_register(&dev, &result) == -ENOMEM && !result);
    stop(&dev); fail_misc = 0; empty();

    b = start(&dev);
    fail_alloc = 1; file.private_data = &b->misc;
    assert(bridge_open(NULL, &file) == -ENOMEM && b->stats.files == 0);
    f = open_(b, &file);
    assert(!backend_connect(f, &token) && token == 1 && b->stats.connections == 1);
    assert(backend_connect(f, &token) == -EIO);
    assert(!bridge_release(NULL, &file) && !b->stats.files && !b->stats.connections);
    stop(&dev); empty();

    /* Auxiliary detach drains live files; late close uses no dead DDK state. */
    b = start(&dev); f = open_(b, &file);
    assert(!backend_connect(f, &token));
    open_(b, &second);
    stop(&dev); ddk_alive = 0;
    assert(!b->online && b->stats.files == 2 && !b->stats.connections && !f->handles);
    assert(!bridge_release(NULL, &file));
    assert(!bridge_release(NULL, &second));
    empty(); ddk_alive = 1;

    /* Failed close stays owned; stop retries before destroying services. */
    b = start(&dev); f = open_(b, &file);
    assert(!backend_connect(f, &token));
    fail_free = 1;
    assert(!bridge_release(NULL, &file));
    assert(!b->stats.files && b->stats.connections == 1 && b->stats.errors == 1);
    stop(&dev); empty();

    /* Failed partial connect cleanup must not lose the native handle base. */
    b = start(&dev); f = open_(b, &file);
    fail_handle = 1; fail_free = 1;
    assert(backend_connect(f, &token) == 17 && f->handles && !b->stats.connections);
    fail_handle = 0;
    assert(!bridge_release(NULL, &file) && !b->stats.connections);
    stop(&dev); empty();

    /* A detach cleanup error retains CPU metadata; late close cannot retry DDK. */
    b = start(&dev); f = open_(b, &file);
    assert(!backend_connect(f, &token));
    fail_free = 1;
    stop(&dev); ddk_alive = 0;
    assert(!b->online && f->handles && b->stats.connections == 1 && b->stats.errors == 1);
    assert(!bridge_release(NULL, &file) && !b->stats.files && f->handles && f->closed);
    assert(allocations == 3 && current_pid.refs == 1);
    /* Test-only reclamation of intentionally retained metadata. */
    ddk_alive = 1;
    assert(!backend_disconnect(f));
    bridge_file_free(f); kref_put(&b->ref, bridge_free);
    empty();

    b = start(&dev); b->next_token = U32_MAX;
    f = open_(b, &file);
    assert(backend_connect(f, &token) == -ENOSPC && !f->handles);
    assert(!bridge_release(NULL, &file));
    stop(&dev); empty();
    assert(freed_bases == 5);
    puts("SGX bridge lifetime tests passed: close, live detach, registration/cleanup faults, token exhaustion");
    return 0;
}
