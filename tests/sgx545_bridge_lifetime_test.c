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
#include "gma3600_sgx_boot.h"
#include "gma3600_sgx_live.h"
#include "gma3600_sgx_mmu.h"

typedef uint32_t u32;
typedef uint64_t u64;
typedef void *IMG_HANDLE;
typedef struct { unsigned count; void *data[64]; int type[64]; } PVRSRV_HANDLE_BASE;
struct gma_sgx_bridge_stats { u32 files, connections, errors, heap_handles, init_running, init_ran, stages, staged_buffers, staged_pages, stage_errors, device_allocations, device_pages, device_errors, native_bindings, native_errors, bm_checks, bm_errors, bm_allocations, bm_pages, boot_owners, parent_leases, owner_errors; struct sgx545_live_boot live; u32 live_phase; };
struct mock_buffer { void *owner; u32 token, bytes, pins; struct gma_sgx_mmu_binding *binding; struct sgx545_content_measure content; };
struct gma_sgx_buffers { struct mock_buffer slots[32]; };
static struct mock_buffer *visiting_buffer;
static int fail_bind, fail_unbind, live_bindings;
int gma3600_sgx_mmu_bind_flags(void *heap, const __u32 *physical, __u32 count,
    __u32 alignment, __u32 flags, struct gma_sgx_mmu_binding *binding)
{
    const u32 cpu_order[] = {0x1000, 0x9000, 0x3000, 0x7000};
    assert(visiting_buffer && visiting_buffer->pins == 1 && count == 4);
    assert(!memcmp(physical, cpu_order, sizeof(cpu_order)));
    if (fail_bind) { fail_bind--; return -ENOMEM; }
    *binding = (struct gma_sgx_mmu_binding){heap, 0xf4000000 + 0x10000 * live_bindings, count * 4096, count, 1, 0, flags};
    assert(!(binding->address & (alignment - 1)));
    visiting_buffer->binding = binding; live_bindings++;
    return 0;
}
int gma3600_sgx_mmu_root_matches(void *heap, __u32 physical)
{ assert(heap && physical == 0x100000); return 0; }
int gma3600_sgx_mmu_verify(struct gma_sgx_mmu_binding *binding, const __u32 *physical,
    __u32 count, __u32 *tables)
{ assert(binding->live && count == 4); *tables = 1; return 0; }
int gma3600_sgx_mmu_unbind(struct gma_sgx_mmu_binding *binding)
{
    if (binding->failed) return -EIO;
    if (fail_unbind) { fail_unbind--; return -EIO; }
    if (binding->live) { assert(live_bindings > 0); live_bindings--; binding->live = 0; }
    return 0;
}
typedef struct { u32 uiAddr; } IMG_DEV_PHYADDR;
typedef struct {
    u32 ui32HeapID;
    IMG_HANDLE hDevMemHeap;
    IMG_DEV_PHYADDR sDevVAddrBase;
    u32 ui32HeapByteSize, ui32Attribs, ui32XTileStride;
} PVRSRV_HEAP_INFO;
typedef struct { IMG_DEV_PHYADDR sPDDevPAddr; PVRSRV_HEAP_INFO asHeapInfo[32]; } SGX_BRIDGE_INFO_FOR_SRVINIT;
enum {PVRSRV_INIT_SERVER_RUNNING, PVRSRV_INIT_SERVER_RAN, PVRSRV_INIT_SERVER_SUCCESSFUL};
#define IMG_TRUE 1
#define IMG_FALSE 0
#define PVRSRV_ERROR_SRV_CONNECT_FAILED 31
#define PVRSRV_ERROR_SRV_DISCONNECT_FAILED 32
static bool init_state[3], privileged = true;
static bool OSProcHasPrivSrvInit(void) { return privileged; }
static bool PVRSRVGetInitServerState(unsigned which) { return init_state[which]; }
static int PVRSRVSetInitServerState(unsigned which, bool state) { init_state[which] = state; return 0; }
static SGX_BRIDGE_INFO_FOR_SRVINIT native_info;
static int fake_device, fake_heap, fake_heap2, info_calls;
static int SGXGetInfoForSrvinitKM(void *device, SGX_BRIDGE_INFO_FOR_SRVINIT *info)
{ assert(device == &fake_device); *info = native_info; info_calls++; return 0; }
typedef struct { void *pMMUHeap; } BM_HEAP;
struct page { uint64_t physical; };
static struct page mmu_pages[4] = {{0x1000}, {0x9000}, {0x3000}, {0x7000}};
static struct page *mmu_page_list[4] = {&mmu_pages[0], &mmu_pages[1], &mmu_pages[2], &mmu_pages[3]};
#define page_to_phys(page) ((page)->physical)
#define upper_32_bits(value) ((value) >> 32)
static void *mmu_buffer_owner;
static unsigned mmu_page_count = 4, mmu_calls;
static int gma_sgx_buffer_visit(struct gma_sgx_buffers *p, void *owner, u32 token,
    int (*visit)(void *, struct page **, u32), void *context)
{
    assert(p);
    unsigned i;
    int ret;
    if (owner == mmu_buffer_owner && token == 999)
        return visit(context, mmu_page_list, mmu_page_count);
    for (i = 0; i < 32; i++) if (p->slots[i].owner == owner && p->slots[i].token == token) {
        visiting_buffer = &p->slots[i];
        ret = visit(context, mmu_page_list, mmu_page_count);
        visiting_buffer = NULL;
        return ret;
    }
    return -ENOENT;
}
int gma3600_sgx_mmu_exercise(void *heap, const __u32 *physical, __u32 count,
    __u32 flags, struct sgx545_mmu_check *out)
{
    const u32 sorted[] = {0x9000, 0x7000, 0x3000, 0x1000};
    assert(heap == &fake_device && count == 4 && flags <= 1);
    assert(!memcmp(physical, sorted, sizeof(sorted)));
    mmu_calls++; out->checked_pages = 4; return 0;
}
struct gma_sgx_init_memory { u32 token, address, bytes, flags; void *linear; };
struct gma_sgx_native_init { struct gma_sgx_init_memory memory[27]; u32 count; };
static void gma_sgx_init_requirements(struct sgx545_init_requirements *out)
{ unsigned i; for (i = 0; i < 9; i++) out->minimum_bytes[i] = 4; }
static int gma_sgx_init_translate(const struct sgx545_init2_in32 *wire,
    const struct gma_sgx_init_memory *memory, u32 count, struct gma_sgx_native_init *out)
{ memcpy(out->memory, memory, count * sizeof(*memory)); out->count = count; return 0; }
static int gma_sgx_init_check(const struct gma_sgx_native_init *out,
    const struct gma_sgx_init_memory *memory, u32 count)
{ assert(out->count == count); return memcmp(out->memory, memory, count * sizeof(*memory)) ? -EIO : 0; }
static int fail_bm;
static int gma3600_sgx_bm_cycle(void *heap, const __u32 *physical, __u32 count,
    void *linear, __u32 bytes, __u32 flags, struct sgx545_bm_check *out)
{
    assert(heap && linear && bytes == 16384 && count == 4 && visiting_buffer && visiting_buffer->pins);
    assert(physical[0] == 0x1000 && physical[1] == 0x9000 && physical[2] == 0x3000 && physical[3] == 0x7000);
    if (fail_bm) { int ret = fail_bm; fail_bm = 0; return ret; }
    out->passes = 2; out->alias_address = 0xf5800000; out->pt_before = out->pt_after = 1; return 0;
}
static bool use_native_bm;
static unsigned bm_live;
static bool gma3600_sgx_native_bm_enabled(void) { return use_native_bm; }
struct mock_bm_owner { struct gma_sgx_mmu_binding *binding; };
static int gma3600_sgx_bm_allocate(void *heap, const __u32 *physical, __u32 count,
    void *linear, __u32 bytes, __u32 flags, __u32 alignment, void **owner, struct gma_sgx_mmu_binding *binding)
{
    BM_HEAP *native_heap = heap;
    struct mock_bm_owner *o;
    int ret;
    assert(!*owner && linear && bytes && use_native_bm);
    ret = gma3600_sgx_mmu_bind_flags(native_heap->pMMUHeap, physical, count, alignment, flags & SGX545_DEVICE_MMU_FLAGS, binding);
    if (ret) return ret;
    o = calloc(1, sizeof(*o)); assert(o); o->binding = binding; *owner = o; bm_live++; return 0;
}
static int gma3600_sgx_bm_verify(void *owner, const __u32 *physical, __u32 count, __u32 *tables)
{ struct mock_bm_owner *o = owner; assert(o && bm_live); return gma3600_sgx_mmu_verify(o->binding, physical, count, tables); }
static int gma3600_sgx_bm_info(void *owner, void *memory, int attach)
{ assert(owner && memory && bm_live); return 0; }
static int gma3600_sgx_bm_release(void **owner)
{
    struct mock_bm_owner *o = *owner;
    int ret;
    assert(o && bm_live); ret = gma3600_sgx_mmu_unbind(o->binding);
    if (ret) return ret;
    free(o); *owner = NULL; bm_live--; return 0;
}
static unsigned native_calls;
static int fail_native;
static int gma3600_sgx_native_cycle(void *device, struct gma_sgx_native_init *snapshot, struct sgx545_native_cycle *out)
{
    assert(device == &fake_device && (snapshot->count == 9 || snapshot->count == 15)); native_calls++;
    if (fail_native) return fail_native;
    out->bound = out->restored = 1; out->flags = SGX545_NATIVE_CCB_ROUNDTRIP;
    out->ccb_bytes = 8192; out->slots = 256;
    return 0;
}
static unsigned native_live, native_stops;
static void *lease_identity;
static int fail_lease_acquire, fail_lease_release;
static bool firmware_enabled;
static bool gma3600_sgx_firmware_boot_enabled(void) { return firmware_enabled; }
int gma_sgx_live_start(struct gma_sgx_live *l, const struct sgx545_init2_info32 *info,
    const struct gma_sgx_boot_memory *memory, unsigned count,
    const struct gma_sgx_live_owner_ops *ops, void *owner, void *identity, void *runtime)
{
    (void)info;(void)memory;assert(count==15 && runtime==&fake_device);
    l->owner_ops=ops;l->owner=owner;l->identity=identity;l->runtime=runtime;l->attached=1;
    assert(!ops->acquire(owner) && !ops->verify(owner));
    l->boot.phase=GMA_SGX_BOOT_RESET;
    l->boot.owned=l->sealed=1;l->boot.state=GMA_SGX_BOOT_QUARANTINED;l->boot.error=-ETIMEDOUT;
    return -ETIMEDOUT;
}
void gma_sgx_live_result(struct gma_sgx_live *l, struct sgx545_live_boot *out)
{ out->state=l->boot.state;out->sealed=l->sealed;out->error=-l->boot.error; }
void gma_sgx_live_detach(struct gma_sgx_live *l)
{ l->attached=0;l->boot.state=GMA_SGX_BOOT_QUARANTINED; }
static int gma3600_sgx_owner_acquire(void *identity, u32 *abi)
{ assert(native_live && !lease_identity); if (fail_lease_acquire) return -EIO; lease_identity = identity; *abi = 3; return 0; }
static int gma3600_sgx_owner_release(void *identity)
{ assert(native_live && lease_identity == identity); if (fail_lease_release) return -EIO; lease_identity = NULL; return 0; }
/* Outer policy seam; actual captured policy is tested in sgx545_boot_test. */
int gma_sgx_boot_plan_build(const struct sgx545_init2_info32 *info,
    const struct gma_sgx_boot_memory *memory, u32 count, struct gma_sgx_boot_plan *out)
{ assert(count == 15 && info->ccb == memory[0].token); memset(out, 0, sizeof(*out)); return 0; }
static int fail_native_end;
static int gma3600_sgx_native_begin(void *device, struct gma_sgx_native_init *snapshot,
    void **runtime, struct sgx545_native_cycle *out)
{
    assert(!*runtime && !native_live);
    int ret = gma3600_sgx_native_cycle(device, snapshot, out);
    if (ret) return ret;
    out->restored = 0; *runtime = &fake_device; native_live = 1;
    return 0;
}
static int gma3600_sgx_native_query(void *runtime, struct sgx545_native_cycle *out)
{
    assert(runtime == &fake_device && native_live && live_bindings >= 9);
    out->bound = 1; out->restored = 0; out->flags = SGX545_NATIVE_CCB_ROUNDTRIP;
    out->ccb_bytes = 8192; out->slots = 256; return 0;
}
static int gma3600_sgx_native_end(void **runtime)
{
    assert(!lease_identity); /* Parent owner retires before native binding. */
    assert(*runtime == &fake_device && native_live && live_bindings >= 9);
    *runtime = NULL; native_live = 0; native_stops++;
    if (fail_native_end) { fail_native_end = 0; return -EUCLEAN; }
    return 0;
}
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
static int mutex_trylock(struct mutex *m) { if (m->locked) return 0; m->locked = 1; return 1; }
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
static int handle_calls, fail_handle_at, fail_heap_release;
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
static void *kcalloc(size_t n, size_t size, int flags) { return kzalloc(n * size, flags); }
static void kfree(void *p);
static struct gma_sgx_buffers *gma_sgx_buffers_create(void)
{ return kzalloc(sizeof(struct gma_sgx_buffers), 0); }
struct vm_area_struct { int unused; };
static int gma_sgx_buffer_mmap(struct gma_sgx_buffers *p, void *owner, struct vm_area_struct *vma)
{ assert(p && owner && vma); return 0; }
static int gma_sgx_buffer_alloc(struct gma_sgx_buffers *p, void *owner, u32 token, u32 flags,
    u32 bytes, struct sgx545_alloc_shared_out *out)
{
    unsigned i;
    for (i = 0; i < 32; i++) if (!p->slots[i].owner) break;
    if (i == 32) return -ENOSPC;
    p->slots[i] = (struct mock_buffer){.owner = owner, .token = token, .bytes = bytes};
    memset(out, 0, sizeof(*out));
    out->kernel_mem = out->mem.kernel_mem = out->mem.mapping = token;
    out->mem.alloc_size = bytes; out->mem.flags = flags | SGX545_BUFFER_MULTI;
    return 0;
}
static int gma_sgx_buffer_pin(struct gma_sgx_buffers *p, void *owner, u32 token, void **pin)
{
    unsigned i;
    for (i = 0; i < 32; i++) if (p->slots[i].owner == owner && p->slots[i].token == token) {
        p->slots[i].pins++; *pin = &p->slots[i]; return 0;
    }
    return -ENOENT;
}
static void gma_sgx_buffer_unpin(void *pin)
{
    struct mock_buffer *buffer = pin;
    assert(buffer->pins == 1 && (!buffer->binding || !buffer->binding->live));
    buffer->pins--;
}
static int gma_sgx_buffer_free(struct gma_sgx_buffers *p, void *owner, u32 token)
{
    unsigned i;
    for (i = 0; i < 32; i++) if (p->slots[i].owner == owner && p->slots[i].token == token) {
        assert(!p->slots[i].pins);
        memset(&p->slots[i], 0, sizeof(p->slots[i])); return 0;
    }
    return -ENOENT;
}
static void gma_sgx_buffers_release_owner(struct gma_sgx_buffers *p, void *owner)
{
    unsigned i;
    assert(p && owner);
    for (i = 0; i < 32; i++) if (p->slots[i].owner == owner) {
        assert(!p->slots[i].pins);
        memset(&p->slots[i], 0, sizeof(p->slots[i]));
    }
}
static void gma_sgx_buffers_stop(struct gma_sgx_buffers *p)
{
    unsigned i;
    for (i = 0; i < 32; i++) if (p->slots[i].pins) return; /* quarantined backing retains pool */
    kfree(p);
}
static int fail_transfer;
static int gma_sgx_buffer_measure(struct gma_sgx_buffers *p, void *owner, u32 token,
    struct sgx545_content_measure *out)
{
    unsigned i;
    for (i = 0; i < 32; i++) if (p->slots[i].owner == owner && p->slots[i].token == token) {
        *out = p->slots[i].content; return 0;
    }
    return -ENOENT;
}
static int gma_sgx_buffer_describe(struct gma_sgx_buffers *p, void *owner, u32 token,
    void **linear, u32 *bytes)
{
    unsigned i;
    for (i = 0; i < 32; i++) if (p->slots[i].owner == owner && p->slots[i].token == token) {
        *linear = (void *)(uintptr_t)(0x100000000ULL + token * 0x10000ULL);
        *bytes = p->slots[i].bytes; return 0;
    }
    return -ENOENT;
}
static int gma_sgx_buffers_transfer(struct gma_sgx_buffers *p, void *from, void *to, const u32 *tokens, u32 count)
{
    unsigned i, j;
    if (fail_transfer) { fail_transfer--; return -EIO; }
    for (i = 0; i < count; i++) {
        for (j = 0; j < 32; j++) if (p->slots[j].owner == from && p->slots[j].token == tokens[i]) break;
        assert(j < 32 && p->slots[j].pins == 1); p->slots[j].owner = to;
    }
    return 0;
}
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
#define PVRSRV_HANDLE_TYPE_DEV_NODE 2
#define PVRSRV_HANDLE_TYPE_DEV_MEM_HEAP 3
#define PVRSRV_HANDLE_ALLOC_FLAG_NONE 0
#define PVRSRV_HANDLE_ALLOC_FLAG_SHARED 1
static int PVRSRVAllocHandleBase(PVRSRV_HANDLE_BASE **base)
{ assert(ddk_alive); *base = kzalloc(sizeof(**base), 0); return *base ? 0 : 12; }
static int PVRSRVAllocHandle(PVRSRV_HANDLE_BASE *base, IMG_HANDLE *handle,
                            void *data, int type, int flags)
{
    unsigned i;
    assert(ddk_alive && base);
    handle_calls++;
    if (fail_handle || handle_calls == fail_handle_at) return 17;
    for (i = 0; i < 64; i++) if (!base->data[i]) break;
    assert(i < 64); base->count++; base->data[i] = data; base->type[i] = type;
    *handle = (void *)(uintptr_t)(i + 1); return 0;
}
static int PVRSRVLookupHandle(PVRSRV_HANDLE_BASE *base, void **data, void *handle, int type)
{
    uintptr_t n = (uintptr_t)handle;
    assert(ddk_alive && base);
    if (!n || n > 64 || !base->data[n - 1] || base->type[n - 1] != type) return 22;
    *data = base->data[n - 1]; return 0;
}
static int PVRSRVReleaseHandle(PVRSRV_HANDLE_BASE *base, void *handle, int type)
{
    void *data;
    uintptr_t n = (uintptr_t)handle;
    if (fail_heap_release) { fail_heap_release--; return 23; }
    assert(!PVRSRVLookupHandle(base, &data, handle, type));
    base->data[n - 1] = NULL; base->count--; return 0;
}
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
    memset(init_state, 0, sizeof(init_state));
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
{ assert(!allocations && !current_pid.refs && !misc_live && !watched_bridge && !bm_live); }

static void attach_device(struct gma_sgx_file *f)
{
    assert(!PVRSRVAllocHandle(f->handles, &f->device_handle, &fake_device,
                             PVRSRV_HANDLE_TYPE_DEV_NODE, PVRSRV_HANDLE_ALLOC_FLAG_SHARED));
}
static void initializer_lifetime(void)
{
    struct device dev = {0};
    struct gma_sgx_bridge *b;
    struct file file, second;
    struct gma_sgx_file *f, *other;
    struct sgx545_init_info_out out, again;
    u32 token;
    unsigned i, old_token;
    int calls;

    memset(&native_info, 0, sizeof(native_info));
    native_info.sPDDevPAddr.uiAddr = 0x100000;
    for (i = 0; i < 32; i++) native_info.asHeapInfo[i].ui32HeapID = UINT32_MAX;
    native_info.asHeapInfo[0] = (PVRSRV_HEAP_INFO){1, &fake_heap, {0x4000000}, 0x2000000, 3, 0};
    native_info.asHeapInfo[1] = (PVRSRV_HEAP_INFO){2, NULL, {0x6000000}, 0x1000000, 1, 0};
    native_info.asHeapInfo[2] = (PVRSRV_HEAP_INFO){3, &fake_heap, {0x7000000}, 0x1000000, 3, 0};
    native_info.asHeapInfo[3] = (PVRSRV_HEAP_INFO){4, &fake_heap2, {0x8000000}, 0x1000000, 3, 4096};
    native_info.asHeapInfo[4] = (PVRSRV_HEAP_INFO){0x07000009, NULL, {0xd2000000}, 0, 0x02024200, 0};

    b = start(&dev); f = open_(b, &file); other = open_(b, &second);
    assert(!backend_connect(f, &token) && !backend_connect(other, &token));
    privileged = false;
    assert(backend_init_connect(f) == -EPERM && !b->init_owner);
    privileged = true;
    assert(!backend_init_connect(f) && init_state[PVRSRV_INIT_SERVER_RUNNING]);
    assert(backend_init_connect(other) == PVRSRV_ERROR_SRV_CONNECT_FAILED);
    assert(backend_init_abort(other, 0) == PVRSRV_ERROR_SRV_DISCONNECT_FAILED && b->init_owner == f);
    assert(!backend_init_abort(f, 1) && !b->init_owner && !init_state[PVRSRV_INIT_SERVER_RAN]);
    assert(!backend_init_connect(f));
    assert(backend_init_info(f, &out) == 22); /* no acquired device handle */
    attach_device(f);

    fail_handle_at = handle_calls + 2;
    memset(&out, 0, sizeof(out));
    assert(backend_init_info(f, &out) == 17 && !out.page_directory);
    assert(!f->info_dirty && !f->heap_count && !b->stats.heap_handles && f->handles->count == 2);
    fail_handle_at = 0;
    assert(!backend_init_info(f, &out) && out.page_directory == 0x100000);
    assert(out.heaps[0].handle && out.heaps[0].handle == out.heaps[2].handle);
    assert(out.heaps[3].handle && out.heaps[3].handle != out.heaps[0].handle);
    assert(!out.heaps[1].handle && out.heaps[1].size == 0x1000000);
    assert(out.heaps[3].tile_stride == 4096);
    assert(out.heaps[4].id == 0x07000009 && out.heaps[4].base == 0xd2000000);
    assert(!out.heaps[4].size && !out.heaps[4].handle && out.heaps[4].attributes == 0x02024200);
    for (i = 5; i < 32; i++) {
        assert(out.heaps[i].id == UINT32_MAX && !out.heaps[i].handle && !out.heaps[i].base);
    }
    assert(f->heap_count == 2 && b->stats.heap_handles == 2 && f->handles->count == 4);
    calls = info_calls; old_token = b->next_token;
    assert(!backend_init_info(f, &again) && !memcmp(&out, &again, sizeof(out)));
    assert(info_calls == calls && b->next_token == old_token);
    assert(!backend_release_init_info(f) && !f->heap_count && !b->stats.heap_handles);

    assert(!backend_init_info(f, &out));
    fail_heap_release = 1;
    assert(backend_release_init_info(f) == 23 && f->info_dirty && f->heap_count == 1);
    assert(backend_init_info(f, &out) == -EIO);
    assert(!backend_release_init_info(f) && !f->info_dirty && !b->stats.heap_handles);
    fail_handle_at = handle_calls + 2; fail_heap_release = 1;
    assert(backend_init_info(f, &out) == -EIO && f->info_dirty && f->heap_count == 1);
    fail_handle_at = 0;
    assert(!backend_release_init_info(f));
    assert(!backend_init_info(f, &out));
    assert(!backend_init_abort(f, 0));
    assert(!b->stats.init_running && b->stats.init_ran && !init_state[PVRSRV_INIT_SERVER_SUCCESSFUL]);
    assert(backend_init_info(f, &out) == -EACCES);
    assert(backend_init_connect(other) == PVRSRV_ERROR_SRV_CONNECT_FAILED);
    assert(!bridge_release(NULL, &file) && !b->stats.heap_handles);
    assert(!bridge_release(NULL, &second));
    stop(&dev); empty();

    /* An owner that closes without an explicit abort cannot strand RUNNING. */
    b = start(&dev); f = open_(b, &file);
    assert(!backend_connect(f, &token)); attach_device(f);
    assert(!backend_init_connect(f) && !backend_init_info(f, &out));
    assert(!bridge_release(NULL, &file));
    assert(!b->stats.init_running && b->stats.init_ran && !b->stats.heap_handles);
    stop(&dev); empty();

    b = start(&dev); f = open_(b, &file);
    assert(!backend_connect(f, &token)); attach_device(f);
    assert(!backend_init_connect(f) && !backend_init_info(f, &out));
    stop(&dev);
    assert(!b->stats.heap_handles && !b->stats.init_running && !f->handles);
    ddk_alive = 0;
    assert(!bridge_release(NULL, &file));
    ddk_alive = 1; empty();
}
static void mmu_bridge_validation(void)
{
    struct device dev = {0};
    struct gma_sgx_bridge *b = start(&dev);
    struct file file;
    struct gma_sgx_file *f = open_(b, &file);
    struct sgx545_mmu_check check = {.services = 1, .device = 2, .heap = 100, .buffer = 999};
    BM_HEAP heap = {.pMMUHeap = &fake_device};
    u32 token;

    assert(!backend_connect(f, &token));
    f->session.services = 1; f->session.device = 2;
    assert(bridge_mmu_check(f, &check) == -EACCES);
    assert(!backend_init_connect(f));
    f->info_ready = true; f->init_info.page_directory=0x100000; mmu_buffer_owner = f;
    f->init_info.heaps[0].handle = 100;
        f->init_info.heaps[0].attributes = SGX545_HEAP_FLAGS;
    assert(!PVRSRVAllocHandle(f->handles, &f->heap_handles[0], &heap,
        PVRSRV_HANDLE_TYPE_DEV_MEM_HEAP, PVRSRV_HANDLE_ALLOC_FLAG_SHARED));
    check.services++;
    assert(bridge_mmu_check(f, &check) == -ENOENT);
    check.services--; check.device++;
    assert(bridge_mmu_check(f, &check) == -ENOENT);
    check.device--; check.heap++;
    assert(bridge_mmu_check(f, &check) == -ENOENT);
    check.heap--; check.flags = 2;
    assert(bridge_mmu_check(f, &check) == -EINVAL);
    check.flags = 0; check.buffer++;
    assert(bridge_mmu_check(f, &check) == -ENOENT);
    check.buffer--; mmu_page_count = 3;
    assert(bridge_mmu_check(f, &check) == -EINVAL);
    mmu_page_count = 4; mmu_pages[0].physical = 1ULL << 32;
    assert(bridge_mmu_check(f, &check) == -EOVERFLOW);
    mmu_pages[0].physical = 0x1000;
    assert(!mmu_calls);
    assert(!bridge_mmu_check(f, &check) && check.checked_pages == 4 && mmu_calls == 1);
    assert(!bridge_release(NULL, &file)); stop(&dev); empty();
}

static void device_allocation_lifetime(void)
{
    struct device dev = {0};
    struct gma_sgx_bridge *b;
    struct file file, second;
    struct gma_sgx_file *f, *other;
    struct sgx545_alloc_device_out a, c;
    struct sgx545_device_check check = {.services = 1, .device = 2};
    BM_HEAP heap = {.pMMUHeap = &fake_device};
    u32 token, first;
    unsigned mode;
    for (mode = 0; mode < 4; mode++) {
        b = start(&dev); f = open_(b, &file); other = open_(b, &second);
        assert(!backend_connect(f, &token) && !backend_connect(other, &token));
        f->session.services = 1; f->session.device = 2;
        assert(backend_alloc_device(f, 100, SGX545_DEVICE_FLAGS, 16384, 4096, &a) == -EACCES);
        assert(!backend_init_connect(f)); f->info_ready = true; f->init_info.page_directory=0x100000;
        f->init_info.heaps[0].handle = 100;
        f->init_info.heaps[0].attributes = SGX545_HEAP_FLAGS;
        assert(!PVRSRVAllocHandle(f->handles, &f->heap_handles[0], &heap,
            PVRSRV_HANDLE_TYPE_DEV_MEM_HEAP, PVRSRV_HANDLE_ALLOC_FLAG_SHARED));
        assert(backend_alloc_device(f, 101, SGX545_DEVICE_FLAGS, 16384, 4096, &a) == -ENOENT);
        f->init_info.heaps[0].attributes = (SGX545_HEAP_FLAGS & ~SGX545_BUFFER_WC) | 0x1000;
        assert(backend_alloc_device(f, 100, SGX545_INTEL_INIT_FLAGS, 16384, 4096, &a) == -EOPNOTSUPP);
        f->init_info.heaps[0].attributes = SGX545_HEAP_FLAGS;
        fail_bind = 1;
        assert(backend_alloc_device(f, 100, SGX545_DEVICE_FLAGS, 16384, 4096, &a) == -ENOMEM);
        assert(!b->stats.device_allocations && !b->stats.device_pages);
        fail_alloc = 1;
        assert(backend_alloc_device(f, 100, SGX545_DEVICE_FLAGS, 16384, 4096, &a) == -ENOMEM);
        assert(!backend_alloc_device(f, 100, SGX545_DEVICE_FLAGS, 16384, 4096, &a)); first = a.kernel_mem;
        assert(a.mem.device_address == 0xf4000000 && b->stats.device_pages == 4);
        assert(!backend_alloc_device(f, 100, SGX545_INTEL_INIT_FLAGS | SGX545_BUFFER_MULTI, 16384, 4096, &c));
        assert(c.mem.flags == (SGX545_INTEL_INIT_FLAGS | SGX545_HEAP_FLAGS));
        assert(f->device_allocations->binding.memory_flags == (SGX545_INTEL_INIT_FLAGS & SGX545_DEVICE_MMU_FLAGS));
        assert(b->stats.device_allocations == 2 && b->stats.device_pages == 8 && live_bindings == 2);
        check.buffer = first;
        assert(!bridge_device_check(f, &check) && check.pages == 4 && check.address == a.mem.device_address);
        check.device++;
        assert(bridge_device_check(f, &check) == -ENOENT); check.device--;
        assert(backend_free_device(other, first) == -ENOENT);
        assert(backend_free_device(other, c.kernel_mem) == -ENOENT); /* explicit MULTI never grants ownership */
        assert(backend_free_shared(f, first) == -EINVAL);
        if (!mode) {
            assert(!backend_free_device(f, first) && b->stats.device_allocations == 1);
            assert(backend_free_device(f, first) == -ENOENT);
            check.buffer = c.kernel_mem;
            assert(!bridge_device_check(f, &check));
            assert(!backend_disconnect(f) && !live_bindings);
        } else if (mode == 1) {
            fail_unbind = 1;
            assert(backend_free_device(f, first) == -EIO);
            assert(b->stats.device_allocations == 2 && b->stats.device_pages == 8 && live_bindings == 2);
            assert(f->info_dirty && b->stats.device_errors == 1);
            assert(backend_alloc_device(f, 100, SGX545_DEVICE_FLAGS, 16384, 4096, &a) == -EIO);
        } else if (mode == 2) {
            stop(&dev); ddk_alive = 0;
            assert(!f->handles && !live_bindings && !b->stats.device_allocations);
        } else {
            assert(!backend_init_abort(f, 0));
            assert(!backend_free_device(f, first)); /* free after abort */
        }
        assert(!bridge_release(NULL, &file) && !live_bindings);
        assert(!bridge_release(NULL, &second));
        ddk_alive = 1;
        if (dev.action) stop(&dev);
        empty();
    }
}

static int publish_stage(void *context, const struct sgx545_init_stage *message)
{ assert(message->stage && message->buffers == 9 && message->pages == 36); return context ? -EFAULT : 0; }
static int publish_runtime(void *context, const struct sgx545_native_runtime *message)
{ assert((message->state.buffers == 9 || message->state.buffers == 15) && message->state.pages == message->state.buffers * 4); return context ? -EFAULT : 0; }
static void initializer_stage_lifetime(void)
{
    unsigned mode, i;
    for (mode = 0; mode < 10; mode++) {
        struct device dev = {0};
        struct gma_sgx_bridge *b = start(&dev);
        struct file first, second;
        struct gma_sgx_file *f = open_(b, &first), *controller = open_(b, &second);
        struct sgx545_init2_in32 wire = {.device = 2};
        struct sgx545_init_stage message = {.services = 1, .device = 2, .request_bytes = sizeof(wire)};
        struct sgx545_init_control control = {0};
        struct sgx545_alloc_device_out allocation;
        struct sgx545_init_requirements caps = {.services = 1, .device = 2};
        BM_HEAP heap = {.pMMUHeap = &fake_device};
        u32 token, tokens[9];
        struct gma_sgx_stage *candidate = NULL;
        struct pid foreign = {0};
        assert(!backend_connect(f, &token));
        assert(!backend_connect(controller, &token)); controller->session.services = token;
        f->session.services = 1; f->session.device = 2;
        assert(stage_requirements(f, &caps) == -EACCES);
        assert(!backend_init_connect(f)); f->info_ready = true; f->init_info.page_directory=0x100000; f->session.initializer = 1;
        assert(!stage_requirements(f, &caps));
        f->init_info.heaps[0].handle = 100;
        f->init_info.heaps[0].attributes = SGX545_HEAP_FLAGS;
        assert(!PVRSRVAllocHandle(f->handles, &f->heap_handles[0], &heap,
            PVRSRV_HANDLE_TYPE_DEV_MEM_HEAP, PVRSRV_HANDLE_ALLOC_FLAG_SHARED));
        for (i = 0; i < 9; i++) {
            u32 flags = i & 1 ? SGX545_INTEL_INIT_FLAGS : SGX545_DEVICE_FLAGS;
            flags = (flags & ~SGX545_DEVICE_ACCESS) | (i % 3 + 1);
            assert(!backend_alloc_device(f, 100, flags, 16384, 4096, &allocation));
            tokens[i] = allocation.kernel_mem;
        }
        wire.info.ccb = tokens[0]; wire.info.ccb_control = tokens[1]; wire.info.event_kicker = tokens[2];
        wire.info.host_control = tokens[3]; wire.info.ta3d_control = tokens[4]; wire.info.misc = tokens[5];
        wire.info.hwperf = tokens[6]; wire.info.ta_signature = tokens[7]; wire.info.signature_3d = tokens[8];
        wire.info.ccb ^= 0x40000000;
        assert(stage_prepare(f, &wire, &message, &candidate) == -ENOENT && !candidate);
        wire.info.ccb ^= 0x40000000;
        {
            struct sgx545_init_stage check = message;
            u32 before = b->next_token;
            assert(!stage_validate(f, &wire, &check));
            assert(!check.stage && check.buffers == 9 && check.pages == 36);
            assert(b->next_token == before && !b->stage && !b->stats.stages);
            assert(b->init_owner == f && f->session.initializer && !b->stats.init_ran);
            assert(b->stats.device_allocations == 9 && live_bindings == 9);
            assert(stage_validate(controller, &wire, &message) == -ENOENT);
            check = message; check.flags = 1;
            assert(stage_validate(f, &wire, &check) == -EINVAL);
            assert(*device_slot(f, tokens[0])); /* ownership stays with the creator */
            b->next_token = U32_MAX; check = message;
            assert(!stage_validate(f, &wire, &check) && b->next_token == U32_MAX);
            b->next_token = before;
        }
        fail_alloc = 1;
        assert(stage_prepare(f, &wire, &message, &candidate) == -ENOMEM && !candidate);
        fail_transfer = 1;
        assert(stage_publish(f, &wire, &message, publish_stage, NULL) == -EIO && !b->stage);
        assert(stage_publish(f, &wire, &message, publish_stage, &dev) == -EFAULT);
        assert(!b->stage && !b->stats.stages && b->stats.device_allocations == 9);
        assert(b->init_owner == f && f->session.initializer && !b->stats.init_ran);
        message.stage = message.buffers = message.pages = 0;
        assert(!stage_publish(f, &wire, &message, publish_stage, NULL));
        assert(b->stage && b->stats.stages == 1 && !f->device_allocations);
        assert(b->stage->memory[1].flags == ((SGX545_INTEL_INIT_FLAGS & ~SGX545_DEVICE_ACCESS) |
            SGX545_DEVICE_WRITE | SGX545_HEAP_FLAGS));
        assert(!b->init_owner && !f->session.initializer && b->stats.init_ran);
        assert(stage_prepare(f, &wire, &message, &candidate) == -EBUSY);
        assert(backend_free_device(f, tokens[0]) == -ENOENT);
        control.services = controller->session.services; control.stage = message.stage;
        controller->owner = &foreign;
        assert(stage_control(controller, &control) == -EPERM); controller->owner = &current_pid;
        assert(!stage_control(controller, &control) && control.buffers == 9 && control.pages == 36);
        attach_device(controller); controller->session.device = 1000;
        {
            struct sgx545_native_cycle native = {.services = controller->session.services, .stage = message.stage, .device = 1000};
            struct mock_buffer *backing = b->stage->allocations->pin;
            unsigned before_calls = native_calls;
            backing->content.mapped_vmas = 1;
            assert(stage_native_cycle(controller, &native) == -EBUSY && native_calls == before_calls);
            backing->content.mapped_vmas = 0; backing->content.hash ^= 1;
            assert(stage_native_cycle(controller, &native) == -EUCLEAN && native_calls == before_calls);
            backing->content.hash ^= 1;
            fail_native = -ENOMEM;
            assert(stage_native_cycle(controller, &native) == -ENOMEM && !b->stage->dirty);
            fail_native = 0;
            assert(!stage_native_cycle(controller, &native) && native.bound && native.restored && native.buffers == 9 && native.pages == 36);
        }
        {
            struct sgx545_init_content content = {.services = controller->session.services, .stage = message.stage};
            struct mock_buffer *backing = b->stage->allocations->pin;
            u32 index;
            assert(!stage_content(controller, &content) && !content.flags && content.token == tokens[0]);
            content = (struct sgx545_init_content){.services = controller->session.services, .stage = message.stage};
            controller->owner = &foreign;
            assert(stage_content(controller, &content) == -EPERM); controller->owner = &current_pid;
            content.index = 9;
            assert(stage_content(controller, &content) == -EINVAL);
            for (index = 0; index < 9 && tokens[index] != backing->token; index++) {}
            assert(index < 9);
            content.index = index; backing->content.hash ^= 1;
            assert(!stage_content(controller, &content) && content.flags == SGX545_CONTENT_CHANGED);
            assert(content.baseline_hash != content.current_hash && !b->stage->dirty);
            backing->content.hash ^= 1;
            content = (struct sgx545_init_content){.services = controller->session.services, .stage = message.stage, .index = index};
            assert(!stage_content(controller, &content) && !content.flags);
            content = (struct sgx545_init_content){.services = controller->session.services, .stage = message.stage, .index = index};
            backing->content.padding_nonzero = 1;
            assert(!stage_content(controller, &content) && content.flags == SGX545_CONTENT_CHANGED &&
                content.baseline_hash == content.current_hash); /* padding is measured separately */
            backing->content.padding_nonzero = 0;
        }
        assert(!bridge_release(NULL, &first) && live_bindings == 9);
        assert(!stage_control(controller, &control)); /* creator and native handles gone */
        if (mode >= 5) {
            struct sgx545_native_runtime runtime = {.state = {.services = controller->session.services,
                .stage = message.stage, .device = controller->session.device}};
            struct sgx545_native_runtime clean = runtime;
            unsigned before_stops = native_stops;
            struct file third;
            struct gma_sgx_file *sibling = open_(b, &third);
            u32 sibling_token;
            struct vm_area_struct blocked_vma = {0};
            assert(!backend_connect(sibling, &sibling_token)); sibling->session.services = sibling_token;
            attach_device(sibling); sibling->session.device = 2000;
            assert(stage_runtime(controller, &runtime, publish_runtime, &dev) == -EFAULT);
            assert(!native_live && !b->stats.native_bindings && b->stage && live_bindings == 9);
            runtime = clean;
            assert(!stage_runtime(controller, &runtime, publish_runtime, NULL) && native_live && b->stats.native_bindings == 1);
            assert(bridge_mmap(&second, &blocked_vma) == -EBUSY);
            runtime = clean; runtime.operation = SGX545_NATIVE_QUERY;
            runtime.state.services = sibling->session.services; runtime.state.device = sibling->session.device;
            assert(stage_runtime(sibling, &runtime, publish_runtime, NULL) == -EPERM);
            assert(!bridge_release(NULL, &third) && native_live && b->stats.native_bindings == 1);
            {
                struct sgx545_bm_check check = {.services = controller->session.services, .stage = message.stage};
                struct sgx545_bm_check prefix = check;
                struct gma_sgx_device_allocation *a = b->stage->allocations;
                check.services++; assert(stage_bm_check(controller, &check) == -ENOENT);
                check = prefix; check.index = 9; assert(stage_bm_check(controller, &check) == -EINVAL);
                check = prefix; check.token = 1; assert(stage_bm_check(controller, &check) == -EINVAL);
                check = prefix; controller->owner = &foreign;
                assert(stage_bm_check(controller, &check) == -EPERM); controller->owner = &current_pid;
                for (i = 0; i < 9; i++) {
                    check = prefix; check.index = i;
                    assert(!stage_bm_check(controller, &check) && check.passes == 2 && check.pages == 4);
                    assert(check.token == tokens[i] && check.original_address != check.alias_address);
                }
                assert(b->stats.bm_checks == 9);
                if (use_native_bm) {
                    struct sgx545_bm_info info = {.services = controller->session.services, .stage = message.stage};
                    assert(b->stats.bm_allocations == 9 && b->stats.bm_pages == 36);
                    assert(!stage_bm_info(controller, &info) && info.owned && info.registered && info.attached);
                    assert(info.token == tokens[0] && info.alignment == 4096 && info.pages == 4);
                }
                check = prefix; fail_bm = -ENOMEM;
                assert(stage_bm_check(controller, &check) == -ENOMEM && !b->stage->dirty && native_live);
                assert(b->stats.bm_errors == 1);
                if (mode == 9) {
                    while (a->token != tokens[0]) a = a->next;
                    check = prefix; fail_bm = -EUCLEAN;
                    assert(stage_bm_check(controller, &check) == -EUCLEAN && b->stage->dirty && a->binding.failed);
                    assert(b->stats.bm_errors == 2 && live_bindings == 9 && native_live);
                    /* Test-only recovery: production quarantines the pinned RAM. */
                    a->binding.failed = 0; b->stage->dirty = false;
                }
            }
            runtime = clean;
            assert(stage_runtime(controller, &runtime, publish_runtime, NULL) == -EBUSY);
            runtime.operation = SGX545_NATIVE_QUERY;
            assert(!stage_runtime(controller, &runtime, publish_runtime, NULL) && runtime.state.bound && !runtime.state.restored);
            runtime = clean; runtime.operation = SGX545_NATIVE_QUERY;
            controller->owner = &foreign;
            assert(stage_runtime(controller, &runtime, publish_runtime, NULL) == -EPERM); controller->owner = &current_pid;
            if (mode == 5) {
                runtime = clean; runtime.operation = SGX545_NATIVE_UNBIND;
                assert(stage_runtime(controller, &runtime, publish_runtime, &dev) == -EFAULT);
                assert(runtime.state.restored && !runtime.state.bound && !native_live && live_bindings == 9);
                runtime = clean; runtime.operation = SGX545_NATIVE_QUERY;
                assert(stage_runtime(controller, &runtime, publish_runtime, NULL) == -ENOENT);
                control.operation = SGX545_STAGE_ABORT; assert(!stage_control(controller, &control));
                assert(!bridge_release(NULL, &second)); stop(&dev);
            } else if (mode == 6) {
                assert(!bridge_release(NULL, &second) && !b->stage && !native_live && !live_bindings);
                stop(&dev);
            } else if (mode == 7) {
                stop(&dev); ddk_alive = 0;
                assert(!b->stage && !native_live && !live_bindings);
                assert(!bridge_release(NULL, &second)); ddk_alive = 1;
            } else {
                control.operation = SGX545_STAGE_ABORT;
                if (mode == 8) fail_unbind = 1; else fail_native_end = 1;
                assert(stage_control(controller, &control) == (mode == 8 ? -EIO : -EUCLEAN));
                assert(b->stats.native_errors == (mode == 9));
                assert(!native_live && !b->stats.native_bindings && b->stage && live_bindings == 9);
                assert(!stage_control(controller, &control));
                assert(!native_live && !live_bindings && !b->stage);
                assert(!bridge_release(NULL, &second)); stop(&dev);
            }
            assert(native_stops == before_stops + 2);
            empty(); continue;
        }
        if (mode == 0 || mode == 1) {
            control.operation = SGX545_STAGE_ABORT;
            if (mode == 1) {
                fail_unbind = 1;
                assert(stage_control(controller, &control) == -EIO && live_bindings == 9);
                assert(b->stats.stages == 1 && b->stats.stage_errors == 1 && b->stage->dirty);
                control.operation = SGX545_STAGE_QUERY;
                assert(stage_control(controller, &control) == -EIO);
                control.operation = SGX545_STAGE_ABORT;
            }
            assert(!stage_control(controller, &control) && !live_bindings && !b->stage);
            assert(stage_control(controller, &control) == -ENOENT);
            assert(!bridge_release(NULL, &second)); stop(&dev);
        } else if (mode == 2) {
            assert(!bridge_release(NULL, &second));
            assert(b->stage && b->stats.files == 0 && live_bindings == 9);
            stop(&dev); assert(!live_bindings);
        } else if (mode == 4) {
            struct gma_sgx_buffers *pool = b->buffers;
            assert(!bridge_release(NULL, &second)); fail_unbind = 1;
            stop(&dev); ddk_alive = 0;
            assert(!b->online && b->stage && b->stage->dirty && b->ref.refs == 1);
            assert(live_bindings == 9 && current_pid.refs == 1);
            /* Test-only recovery of intentionally quarantined CPU metadata.
             * Production never retries native cleanup after DDK teardown. */
            ddk_alive = 1; kref_get(&b->ref);
            assert(!stage_abort(b) && !live_bindings);
            gma_sgx_buffers_stop(pool); kref_put(&b->ref, bridge_free);
        } else {
            stop(&dev); ddk_alive = 0;
            assert(!b->stage && !live_bindings);
            assert(!bridge_release(NULL, &second)); ddk_alive = 1;
        }
        empty();
    }
}

static int publish_owner(void *context, const struct sgx545_boot_owner *message)
{
    assert(message->operation == SGX545_OWNER_RELEASE ? !message->held : message->held == 1);
    return context ? -EFAULT : 0;
}
static int publish_live(void *context, const struct sgx545_live_boot *message)
{ assert(message->sealed==1 && message->error==ETIMEDOUT);return context ? -EFAULT : 0; }
static int publish_fifteen(void *context, const struct sgx545_init_stage *message)
{ (void)context; assert(message->buffers == 15 && message->pages == 60); return 0; }
static void boot_owner_lifetime(void)
{
    unsigned mode, i;
    for (mode = 0; mode < 7; mode++) {
        struct device dev = {0}; struct file file;
        struct gma_sgx_bridge *b = start(&dev);
        struct gma_sgx_file *f = open_(b, &file);
        struct sgx545_init2_in32 wire = {.device=2};
        struct sgx545_init_stage stage = {.services=1,.device=2,.request_bytes=sizeof(wire)};
        struct sgx545_alloc_device_out allocation;
        struct sgx545_native_runtime runtime = {.state={.services=1,.device=2}};
        struct sgx545_boot_owner message = {.services=1,.device=2}, clean;
        BM_HEAP heap = {.pMMUHeap=&fake_device}; u32 token, tokens[15];
        assert(!backend_connect(f, &token)); f->session.services=1; f->session.device=2; attach_device(f);
        assert(!backend_init_connect(f)); f->info_ready = true; f->init_info.page_directory=0x100000; f->session.initializer=1;
        f->init_info.heaps[0].handle=100; f->init_info.heaps[0].attributes=SGX545_HEAP_FLAGS;
        assert(!PVRSRVAllocHandle(f->handles, &f->heap_handles[0], &heap,
            PVRSRV_HANDLE_TYPE_DEV_MEM_HEAP, PVRSRV_HANDLE_ALLOC_FLAG_SHARED));
        for (i=0;i<15;i++) {
            assert(!backend_alloc_device(f,100,SGX545_INTEL_INIT_FLAGS,16384,4096,&allocation)); tokens[i]=allocation.kernel_mem;
        }
        wire.info.ccb=tokens[0];wire.info.ccb_control=tokens[1];wire.info.event_kicker=tokens[2];
        wire.info.host_control=tokens[3];wire.info.ta3d_control=tokens[4];wire.info.misc=tokens[5];
        wire.info.hwperf=tokens[6];wire.info.ta_signature=tokens[7];wire.info.signature_3d=tokens[8];
        for(i=9;i<15;i++)wire.info.mem_handles[i-9]=tokens[i];
        assert(!stage_publish(f,&wire,&stage,publish_fifteen,NULL));
        runtime.state.stage=message.stage=stage.stage; clean=message;
        assert(!stage_runtime(f,&runtime,publish_runtime,NULL));
        fail_lease_acquire=1;
        assert(stage_boot_owner(f,&message,publish_owner,NULL)==-EIO && !lease_identity && !b->stage->boot_plan);
        fail_lease_acquire=0; message=clean;
        assert(stage_boot_owner(f,&message,publish_owner,&dev)==-EFAULT && !lease_identity && !b->stats.boot_owners);
        message=clean; assert(!stage_boot_owner(f,&message,publish_owner,NULL));
        assert(lease_identity==b->stage && b->stats.boot_owners==1 && b->stats.parent_leases==1);
        assert(message.parent_abi==3 && message.buffers==15 && message.pages==60 && message.verified);
        message=clean; message.operation=SGX545_OWNER_QUERY;
        assert(stage_boot_owner(f,&message,publish_owner,&dev)==-EFAULT && lease_identity && native_live);
        message=clean; message.operation=SGX545_OWNER_QUERY;
        assert(!stage_boot_owner(f,&message,publish_owner,NULL));
        message=clean; assert(stage_boot_owner(f,&message,publish_owner,NULL)==-EALREADY);
        runtime=(struct sgx545_native_runtime){.state={.services=1,.device=2,.stage=stage.stage},.operation=SGX545_NATIVE_UNBIND};
        assert(stage_runtime(f,&runtime,publish_runtime,NULL)==-EBUSY && native_live);
        if(mode>=5) {
            struct sgx545_live_boot live={.services=1,.device=2,.stage=stage.stage}, query;
            message=clean;message.operation=SGX545_OWNER_RELEASE;
            assert(!stage_boot_owner(f,&message,publish_owner,NULL));
            assert(stage_live(f,&live,publish_live,NULL)==-EOPNOTSUPP && !b->stage->live);
            firmware_enabled=true;
            assert(stage_live(f,&live,publish_live,mode==6 ? &dev : NULL)==(mode==6 ? -EFAULT : 0));
            assert(lease_identity && native_live && b->stage->live->sealed);
            assert(b->stats.live_phase == GMA_SGX_BOOT_RESET);
            query=(struct sgx545_live_boot){.services=1,.device=2,.stage=stage.stage,.operation=SGX545_LIVE_QUERY};
            assert(!stage_live(f,&query,publish_live,NULL));
            assert(b->stats.live_phase == GMA_SGX_BOOT_RESET);
            query=(struct sgx545_live_boot){.services=1,.device=2,.stage=stage.stage};
            assert(stage_live(f,&query,publish_live,NULL)==-EALREADY);
            assert(stage_abort(b)==-EBUSY && !b->stats.stage_errors && !b->stats.owner_errors);
            assert(!bridge_release(NULL,&file) && native_live && lease_identity);
            stop(&dev);
            assert(!b->online && native_live && live_bindings==15 && !b->stage->live->attached);
            /* Test-only teardown of mocked hardware. A production sealed
             * owner has no unseal path and remains retained until reboot. */
            b->stage->live->sealed=b->stage->live->boot.owned=0;
            bridge_stop(b);firmware_enabled=false;empty();continue;
        }
        if(mode==0) {
            message=clean;message.operation=SGX545_OWNER_RELEASE;
            assert(stage_boot_owner(f,&message,publish_owner,&dev)==-EFAULT && !lease_identity && native_live);
            message=clean; assert(!stage_boot_owner(f,&message,publish_owner,NULL));
            message=clean;message.operation=SGX545_OWNER_RELEASE;
            assert(!stage_boot_owner(f,&message,publish_owner,NULL) && !lease_identity && native_live);
        } else if(mode==1) {
            fail_lease_release=1;
            assert(stage_abort(b)==-EIO && native_live && live_bindings==15 && b->stage->parent_leased);
            assert(b->stats.owner_errors==1); fail_lease_release=0;
        } else if(mode==2) {
            stop(&dev); assert(!native_live && !lease_identity && !b->stage);
        } else if(mode==4) {
            fail_lease_release=1; stop(&dev);
            assert(!b->online && lease_identity && native_live && live_bindings==15);
            assert(!bridge_release(NULL,&file));
            assert(lease_identity && native_live && live_bindings==15);
            /* Test-only recovery while the mocked common DDK is still live.
             * Real failed removal retains its owner/context until reboot. */
            fail_lease_release=0; bridge_stop(b); empty(); continue;
        }
        /* Explicit owner retirement, failed-retirement recovery, forced
         * detach and last-close all retire parent before native/PTE backing. */
        assert(!bridge_release(NULL,&file));
        assert(!lease_identity && !native_live && !live_bindings);
        if(dev.action)stop(&dev);
        empty();
    }
}

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
    assert(!PVRSRVFreeHandleBase(f->handles)); /* pool already stopped */
    f->handles = NULL; b->stats.connections--;
    bridge_file_free(f); kref_put(&b->ref, bridge_free);
    empty();

    b = start(&dev); b->next_token = U32_MAX;
    f = open_(b, &file);
    assert(backend_connect(f, &token) == -ENOSPC && !f->handles);
    assert(!bridge_release(NULL, &file));
    stop(&dev); empty();
    assert(freed_bases == 5);
    {
        struct vm_area_struct vma = {0};
        b = start(&dev); f = open_(b, &file);
        mutex_lock(&b->lock);
        assert(bridge_mmap(&file, &vma) == -EAGAIN); /* mmap_lock must not wait for an ioctl's copy fault */
        mutex_unlock(&b->lock);
        assert(!bridge_mmap(&file, &vma));
        stop(&dev);
        assert(bridge_mmap(&file, &vma) == -ENODEV);
        assert(!bridge_release(NULL, &file)); empty();
    }
    initializer_lifetime();
    mmu_bridge_validation();
    device_allocation_lifetime();
    initializer_stage_lifetime();
    use_native_bm = true;
    device_allocation_lifetime();
    initializer_stage_lifetime();
    boot_owner_lifetime();
    use_native_bm = false;
    puts("SGX bridge lifetime tests passed: ownership, heap translation/rollback, device allocation/pin ordering, failed unmap retention, close and detach");
    return 0;
}
