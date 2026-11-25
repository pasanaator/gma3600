// SPDX-License-Identifier: GPL-2.0-only
/* Restricted legacy-wire diagnostic transport. GPU commands are not exposed. */
#include <linux/compat.h>
#include <linux/fs.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "services_headers.h"
#include "mutex.h"
#include "lock.h"
#include "handle.h"
#include "pvr_bridge.h"
#include "sgx_bridge.h"
#include "sgxinfo.h"
#include "sgxinfokm.h"
#include "sgx_bridge_km.h"
#include "power.h"
#include "sgx545_abi.h"
#include "gma3600_sgx_bridge.h"
#include "gma3600_sgx_buffers.h"
#include "gma3600_sgx_mmu.h"
#include "buffer_manager.h"
#include "gma3600_sgx_init.h"

#if !defined(PVR_SECURE_HANDLES)
#error The diagnostic bridge requires DDK secure handles
#endif
#if defined(PDUMP)
#error PDump CONNECT flags need a real implementation before enabling PDUMP
#endif
static_assert(SRV_FLAGS_PERSIST == SGX545_CONNECT_PDUMP_PERSIST);
static_assert(SRV_FLAGS_PDUMP_ACTIVE == SGX545_CONNECT_PDUMP_ACTIVE);
static_assert(PVRSRV_MAX_DEVICES == SGX545_MAX_DEVICES);
static_assert(sizeof(PVRSRV_DEVICE_IDENTIFIER) == sizeof(struct sgx545_device_id));
static_assert(sizeof(PVRSRV_BRIDGE_OUT_CONNECT_SERVICES) == 16);
static_assert(offsetof(PVRSRV_BRIDGE_OUT_CONNECT_SERVICES, hKernelServices) == 8);
static_assert(sizeof(struct sgx545_connect_out) == 12);
static_assert(_IOC_NR(PVRSRV_BRIDGE_CONNECT_SERVICES) == SGX545_CMD_CONNECT);
static_assert(_IOC_NR(PVRSRV_BRIDGE_DISCONNECT_SERVICES) == SGX545_CMD_DISCONNECT);
static_assert(_IOC_NR(PVRSRV_BRIDGE_ENUM_DEVICES) == SGX545_CMD_ENUMERATE);
static_assert(_IOC_NR(PVRSRV_BRIDGE_ACQUIRE_DEVICEINFO) == SGX545_CMD_ACQUIRE);
static_assert(_IOC_NR(PVRSRV_BRIDGE_INITSRV_CONNECT) == SGX545_CMD_INIT_CONNECT);
static_assert(_IOC_NR(PVRSRV_BRIDGE_INITSRV_DISCONNECT) == SGX545_CMD_INIT_DISCONNECT);
static_assert(_IOC_NR(PVRSRV_BRIDGE_SGXINFO_FOR_SRVINIT) == SGX545_CMD_INIT_INFO);
static_assert(PVRSRV_MAX_CLIENT_HEAPS == SGX545_MAX_HEAPS);
static_assert(sizeof(IMG_DEV_PHYADDR) == 4);
static_assert(sizeof(PVRSRV_HEAP_INFO) == 32);
static_assert(sizeof(struct sgx545_heap32) == 24);
static_assert(sizeof(struct sgx545_init_info_out) == 776);
static_assert(_IOC_NR(PVRSRV_BRIDGE_ALLOC_SHARED_SYS_MEM) == SGX545_CMD_ALLOC_SHARED);
static_assert(_IOC_NR(PVRSRV_BRIDGE_FREE_SHARED_SYS_MEM) == SGX545_CMD_FREE_SHARED);
static_assert(_IOC_NR(PVRSRV_BRIDGE_MHANDLE_TO_MMAP_DATA) == SGX545_CMD_MAP_INFO);
static_assert(SGX545_BUFFER_WC == PVRSRV_HAP_WRITECOMBINE);
static_assert(SGX545_BUFFER_MULTI == PVRSRV_HAP_MULTI_PROCESS);
static_assert(_IOC_SIZE(SGX545_IOCTL_INIT_REQUIREMENTS) == sizeof(struct sgx545_init_requirements));
static_assert(_IOC_SIZE(SGX545_IOCTL_INIT_STAGE) == sizeof(struct sgx545_init_stage));
static_assert(_IOC_SIZE(SGX545_IOCTL_INIT_CONTROL) == sizeof(struct sgx545_init_control));

struct gma_sgx_bridge {
	struct miscdevice misc;
	struct kref ref;
	struct mutex lock;
	struct list_head files;
	struct gma_sgx_bridge_stats stats;
	struct gma_sgx_file *init_owner;
	struct gma_sgx_buffers *buffers;
	struct gma_sgx_stage *stage;
	u32 next_token;
	bool online, registered;
};

/* A pin holds the CPU backing until all native PTEs have been removed. */
struct gma_sgx_device_allocation {
	struct gma_sgx_device_allocation *next;
	struct gma_sgx_mmu_binding binding;
	void *pin;
	u32 token;
};
struct device_visit_context {
	struct gma_sgx_device_allocation *allocation;
	BM_HEAP *heap;
	u32 alignment, tables;
	bool verify;
};

struct gma_sgx_file {
	struct gma_sgx_bridge *bridge;
	struct list_head link;
	struct pid *owner;
	struct sgx545_session session;
	PVRSRV_HANDLE_BASE *handles;
	IMG_HANDLE service_handle, device_handle;
	bool counted, closed;
	IMG_HANDLE heap_handles[SGX545_MAX_HEAPS];
	struct sgx545_init_info_out init_info;
	u32 heap_count;
	bool info_ready, info_dirty;
	struct gma_sgx_device_allocation *device_allocations;
};

struct gma_sgx_stage {
	struct gma_sgx_native_init snapshot;
	struct gma_sgx_init_memory memory[SGX545_INIT_MAX_BUFFERS];
	struct gma_sgx_device_allocation *allocations;
	struct pid *owner;
	u32 id, count, tokens[SGX545_INIT_MAX_BUFFERS];
	bool dirty;
};

static void bridge_free(struct kref *ref);

/* No native pointer/handle is exposed. Wire IDs never repeat in this lifetime. */
static int new_token(struct gma_sgx_bridge *b, u32 *token)
{
	if (b->next_token == U32_MAX)
		return -ENOSPC;
	*token = ++b->next_token;
	return 0;
}

/* Backend calls run with bridge->lock and the DDK bridge mutex held. */
static int backend_connect(void *ctx, u32 *token)
{
	struct gma_sgx_file *f = ctx;
	int ret;

	if (f->handles)
		return -EIO;
	ret = new_token(f->bridge, token);
	if (ret)
		return ret;
	ret = PVRSRVAllocHandleBase(&f->handles);
	if (ret)
		return ret;
	ret = PVRSRVAllocHandle(f->handles, &f->service_handle, f,
			       PVRSRV_HANDLE_TYPE_PERPROC_DATA, PVRSRV_HANDLE_ALLOC_FLAG_NONE);
	if (ret) {
		if (PVRSRVFreeHandleBase(f->handles))
			f->bridge->stats.errors++;
		else
			f->handles = NULL;
		return ret;
	}
	f->bridge->stats.connections++;
	f->counted = true;
	return 0;
}

static int backend_enumerate(void *ctx, struct sgx545_enumerate_out *out)
{
	PVRSRV_DEVICE_IDENTIFIER ids[PVRSRV_MAX_DEVICES];
	u32 count, i;
	int ret;

	ret = PVRSRVEnumerateDevicesKM(&count, ids);
	if (ret)
		return ret;
	if (count > SGX545_MAX_DEVICES)
		return -EOVERFLOW;
	out->count = count;
	for (i = 0; i < count; i++) {
		out->devices[i].type = ids[i].eDeviceType;
		out->devices[i].device_class = ids[i].eDeviceClass;
		out->devices[i].index = ids[i].ui32DeviceIndex;
	}
	return 0;
}

static int backend_acquire(void *ctx, u32 index, u32 type, u32 *token)
{
	struct gma_sgx_file *f = ctx;
	IMG_HANDLE device;
	int ret;

	if (!f->handles || f->device_handle || type != PVRSRV_DEVICE_TYPE_SGX)
		return -EINVAL;
	ret = new_token(f->bridge, token);
	if (ret)
		return ret;
	/* UNKNOWN selects by index; selecting by type would ignore a bad index. */
	ret = PVRSRVAcquireDeviceDataKM(index, PVRSRV_DEVICE_TYPE_UNKNOWN, &device);
	if (ret)
		return ret;
	return PVRSRVAllocHandle(f->handles, &f->device_handle, device,
				 PVRSRV_HANDLE_TYPE_DEV_NODE, PVRSRV_HANDLE_ALLOC_FLAG_SHARED);
}

static int backend_release_device(void *ctx)
{
	struct gma_sgx_file *f = ctx;
	int ret = PVRSRVReleaseHandle(f->handles, f->device_handle, PVRSRV_HANDLE_TYPE_DEV_NODE);

	if (!ret)
		f->device_handle = NULL;
	else
		f->bridge->stats.errors++;
	return ret;
}

static int backend_init_connect(void *ctx)
{
	struct gma_sgx_file *f = ctx;
	struct gma_sgx_bridge *b = f->bridge;

	if (!OSProcHasPrivSrvInit())
		return -EPERM;
	if (!f->handles || b->init_owner ||
	    PVRSRVGetInitServerState(PVRSRV_INIT_SERVER_RUNNING) ||
	    PVRSRVGetInitServerState(PVRSRV_INIT_SERVER_RAN))
		return PVRSRV_ERROR_SRV_CONNECT_FAILED;
	PVRSRVSetInitServerState(PVRSRV_INIT_SERVER_RUNNING, IMG_TRUE);
	b->init_owner = f;
	b->stats.init_running = 1;
	return 0;
}

static int backend_init_abort(void *ctx, int rollback)
{
	struct gma_sgx_file *f = ctx;
	struct gma_sgx_bridge *b = f->bridge;

	if (b->init_owner != f)
		return PVRSRV_ERROR_SRV_DISCONNECT_FAILED;
	/* No SysFinalise/power/firmware path. A delivered attempt is one-shot. */
	PVRSRVSetInitServerState(PVRSRV_INIT_SERVER_RUNNING, IMG_FALSE);
	if (!rollback) {
		PVRSRVSetInitServerState(PVRSRV_INIT_SERVER_RAN, IMG_TRUE);
		PVRSRVSetInitServerState(PVRSRV_INIT_SERVER_SUCCESSFUL, IMG_FALSE);
		b->stats.init_ran = 1;
	}
	b->stats.init_running = 0;
	b->init_owner = NULL;
	return 0;
}

static int backend_release_init_info(void *ctx)
{
	struct gma_sgx_file *f = ctx;
	unsigned int i;
	int ret, error = 0;

	for (i = 0; i < SGX545_MAX_HEAPS; i++) {
		if (!f->heap_handles[i])
			continue;
		ret = PVRSRVReleaseHandle(f->handles, f->heap_handles[i],
					 PVRSRV_HANDLE_TYPE_DEV_MEM_HEAP);
		if (ret) {
			f->bridge->stats.errors++;
			error = ret;
			continue;
		}
		f->heap_handles[i] = NULL;
		f->heap_count--;
		f->bridge->stats.heap_handles--;
	}
	f->info_ready = false;
	f->info_dirty = !!error;
	if (!error)
		memset(&f->init_info, 0, sizeof(f->init_info));
	return error;
}

static int backend_init_info(void *ctx, struct sgx545_init_info_out *out)
{
	struct gma_sgx_file *f = ctx;
	SGX_BRIDGE_INFO_FOR_SRVINIT info = {0};
	IMG_HANDLE device;
	unsigned int i, j;
	int ret;

	if (f->bridge->init_owner != f)
		return -EACCES;
	if (f->info_dirty)
		return -EIO;
	if (f->info_ready) {
		*out = f->init_info;
		return 0;
	}
	ret = PVRSRVLookupHandle(f->handles, &device, f->device_handle,
				 PVRSRV_HANDLE_TYPE_DEV_NODE);
	if (ret)
		return ret;
	ret = SGXGetInfoForSrvinitKM(device, &info);
	if (ret)
		return ret;
	f->init_info.page_directory = info.sPDDevPAddr.uiAddr;
	for (i = 0; i < SGX545_MAX_HEAPS; i++) {
		const PVRSRV_HEAP_INFO *heap = &info.asHeapInfo[i];
		struct sgx545_heap32 *wire = &f->init_info.heaps[i];

		wire->id = heap->ui32HeapID;
		if (wire->id == SGX545_UNDEFINED_HEAP)
			continue;
		wire->base = heap->sDevVAddrBase.uiAddr;
		wire->size = heap->ui32HeapByteSize;
		wire->attributes = heap->ui32Attribs;
		wire->tile_stride = heap->ui32XTileStride;
		if (!heap->hDevMemHeap)
			continue;
		/* Shared native heaps get one owned handle and one opaque wire ID. */
		for (j = 0; j < i; j++)
			if (info.asHeapInfo[j].ui32HeapID != SGX545_UNDEFINED_HEAP &&
			    info.asHeapInfo[j].hDevMemHeap == heap->hDevMemHeap)
				break;
		if (j < i) {
			wire->handle = f->init_info.heaps[j].handle;
			continue;
		}
		ret = new_token(f->bridge, &wire->handle);
		if (ret)
			goto fail;
		ret = PVRSRVAllocHandle(f->handles, &f->heap_handles[i], heap->hDevMemHeap,
					PVRSRV_HANDLE_TYPE_DEV_MEM_HEAP, PVRSRV_HANDLE_ALLOC_FLAG_SHARED);
		if (ret)
			goto fail;
		f->heap_count++;
		f->bridge->stats.heap_handles++;
	}
	f->info_ready = true;
	*out = f->init_info;
	return 0;
fail:
	if (backend_release_init_info(f))
		return -EIO;
	return ret;
}

static struct gma_sgx_device_allocation **device_slot(struct gma_sgx_file *f, u32 token)
{
	struct gma_sgx_device_allocation **slot = &f->device_allocations;

	while (*slot && (*slot)->token != token)
		slot = &(*slot)->next;
	return slot;
}

static int device_visit(void *context, struct page **pages, u32 count)
{
	struct device_visit_context *ctx = context;
	u32 *physical, i;
	int ret;

	if (!count || count > SGX545_BUFFER_MAX_BYTES / 4096)
		return -EINVAL;
	physical = kcalloc(count, sizeof(*physical), GFP_KERNEL);
	if (!physical)
		return -ENOMEM;
	/* CPU page order is essential: a GPU VA must name the same buffer byte. */
	for (i = 0; i < count; i++) {
		if (upper_32_bits(page_to_phys(pages[i]))) {
			kfree(physical);
			return -EOVERFLOW;
		}
		physical[i] = page_to_phys(pages[i]);
	}
	if (ctx->verify)
		ret = gma3600_sgx_mmu_verify(&ctx->allocation->binding, physical, count, &ctx->tables);
	else
		ret = gma3600_sgx_mmu_bind(ctx->heap->pMMUHeap, physical, count,
			ctx->alignment, &ctx->allocation->binding);
	kfree(physical);
	return ret;
}

static int device_free_owned(struct gma_sgx_bridge *b, void *owner,
	struct gma_sgx_device_allocation **slot)
{
	struct gma_sgx_device_allocation *allocation = *slot;
	int ret;

	ret = gma3600_sgx_mmu_unbind(&allocation->binding);
	if (ret) {
		b->stats.device_errors++;
		return ret; /* Retain both the RAM pin and the native bookkeeping. */
	}
	*slot = allocation->next;
	b->stats.device_allocations--;
	b->stats.device_pages -= allocation->binding.bytes / 4096;
	gma_sgx_buffer_unpin(allocation->pin);
	ret = gma_sgx_buffer_free(b->buffers, owner, allocation->token);
	kfree(allocation);
	return ret;
}

static int backend_free_device(void *context, u32 token)
{
	struct gma_sgx_file *f = context;
	struct gma_sgx_device_allocation **slot = device_slot(f, token);
	int ret;

	if (!*slot) return -ENOENT;
	ret = device_free_owned(f->bridge, f, slot);
	if (ret) f->info_dirty = true;
	return ret;
}

static int backend_alloc_device(void *context, u32 heap_token, u32 flags, u32 bytes,
	u32 alignment, struct sgx545_alloc_device_out *out)
{
	struct gma_sgx_file *f = context;
	struct gma_sgx_device_allocation *allocation;
	struct device_visit_context visit = {.alignment = alignment};
	struct sgx545_alloc_shared_out backing;
	u32 i, token;
	int ret;

	if (f->bridge->init_owner != f || !f->info_ready)
		return -EACCES;
	if (f->info_dirty)
		return -EIO;
	for (i = 0; i < SGX545_MAX_HEAPS; i++)
		if (f->init_info.heaps[i].handle && f->init_info.heaps[i].handle == heap_token)
			break;
	if (i == SGX545_MAX_HEAPS || !f->heap_handles[i])
		return -ENOENT;
	ret = PVRSRVLookupHandle(f->handles, (void **)&visit.heap, f->heap_handles[i],
		PVRSRV_HANDLE_TYPE_DEV_MEM_HEAP);
	if (ret)
		return -EIO;
	allocation = kzalloc(sizeof(*allocation), GFP_KERNEL);
	if (!allocation)
		return -ENOMEM;
	ret = new_token(f->bridge, &token);
	if (ret)
		goto fail;
	ret = gma_sgx_buffer_alloc(f->bridge->buffers, f, token, SGX545_BUFFER_WC, bytes, &backing);
	if (ret)
		goto fail;
	ret = gma_sgx_buffer_pin(f->bridge->buffers, f, token, &allocation->pin);
	if (ret) {
		gma_sgx_buffer_free(f->bridge->buffers, f, token);
		goto fail;
	}
	allocation->token = token;
	allocation->next = f->device_allocations;
	f->device_allocations = allocation;
	f->bridge->stats.device_allocations++;
	visit.allocation = allocation;
	ret = gma_sgx_buffer_visit(f->bridge->buffers, f, token, device_visit, &visit);
	/* Account only reservations actually obtained, including quarantined ones. */
	f->bridge->stats.device_pages += allocation->binding.bytes / 4096;
	if (ret) {
		if (backend_free_device(f, token))
			return -EIO;
		return ret;
	}
	memset(out, 0, sizeof(*out));
	out->kernel_mem = token;
	out->mem = backing.mem;
	out->mem.device_address = allocation->binding.address;
	out->mem.flags = flags | SGX545_RAM_BACKED | SGX545_BUFFER_MULTI;
	return 0;
fail:
	kfree(allocation);
	return ret;
}

static int stage_requirements(struct gma_sgx_file *f, struct sgx545_init_requirements *out)
{
	if (!f->session.services || out->services != f->session.services ||
	    !f->session.device || out->device != f->session.device) return -ENOENT;
	if (f->bridge->init_owner != f || !f->info_ready) return -EACCES;
	gma_sgx_init_requirements(out);
	return 0;
}

static int stage_prepare(struct gma_sgx_file *f, const struct sgx545_init2_in32 *wire,
	struct sgx545_init_stage *message, struct gma_sgx_stage **result)
{
	struct gma_sgx_bridge *b = f->bridge;
	struct gma_sgx_stage *s;
	u32 i;
	int ret;

	*result = NULL;
	if (!f->session.services || message->services != f->session.services ||
	    !f->session.device || message->device != f->session.device || wire->device != f->session.device)
		return -ENOENT;
	if (!OSProcHasPrivSrvInit()) return -EPERM;
	if (b->stage) return -EBUSY;
	if (b->init_owner != f || !f->info_ready || !f->session.initializer) return -EACCES;
	if (f->info_dirty) return -EIO;
	if (message->flags || message->stage || message->buffers || message->pages ||
	    message->request_bytes != sizeof(*wire)) return -EINVAL;
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s) return -ENOMEM;
	ret = gma_sgx_init_tokens(wire, s->tokens, &s->count);
	if (ret) goto fail;
	for (i = 0; i < s->count; i++) {
		struct gma_sgx_device_allocation *a = *device_slot(f, s->tokens[i]);
		struct device_visit_context visit = {.allocation = a, .verify = true};

		if (!a) { ret = -ENOENT; goto fail; }
		ret = gma_sgx_buffer_visit(b->buffers, f, a->token, device_visit, &visit);
		if (ret) goto fail;
		s->memory[i].token = a->token; s->memory[i].address = a->binding.address;
		s->memory[i].flags = SGX545_DEVICE_FLAGS | SGX545_RAM_BACKED | SGX545_BUFFER_MULTI;
		ret = gma_sgx_buffer_describe(b->buffers, f, a->token,
			&s->memory[i].linear, &s->memory[i].bytes);
		if (ret) goto fail;
	}
	ret = gma_sgx_init_translate(wire, s->memory, s->count, &s->snapshot);
	if (ret) goto fail;
	ret = new_token(b, &s->id);
	if (ret) goto fail;
	*result = s;
	return 0;
fail:
	kfree(s);
	return ret;
}

static void stage_drop_metadata(struct gma_sgx_bridge *b)
{
	struct gma_sgx_stage *s = b->stage;

	b->stage = NULL;
	b->stats.stages = b->stats.staged_buffers = b->stats.staged_pages = 0;
	put_pid(s->owner);
	kfree(s);
	kref_put(&b->ref, bridge_free);
}

static int stage_commit(struct gma_sgx_file *f, struct gma_sgx_stage *s)
{
	struct gma_sgx_bridge *b = f->bridge;
	u32 i;
	int ret;

	ret = gma_sgx_buffers_transfer(b->buffers, f, s, s->tokens, s->count);
	if (ret) return ret;
	/* The bridge mutex prevents owner changes between validation and this move. */
	for (i = 0; i < s->count; i++) {
		struct gma_sgx_device_allocation **slot = device_slot(f, s->tokens[i]), *a = *slot;

		*slot = a->next; a->next = s->allocations; s->allocations = a;
		b->stats.staged_pages += a->binding.pages;
	}
	s->owner = get_pid(f->owner);
	b->stage = s;
	b->stats.stages = 1; b->stats.staged_buffers = s->count;
	kref_get(&b->ref); /* Quarantined cleanup must retain its owning bridge. */
	return 0;
}

static int stage_rollback(struct gma_sgx_file *f)
{
	struct gma_sgx_bridge *b = f->bridge;
	struct gma_sgx_stage *s = b->stage;
	int ret;

	ret = gma_sgx_buffers_transfer(b->buffers, s, f, s->tokens, s->count);
	if (ret) { f->info_dirty = s->dirty = true; b->stats.stage_errors++; return ret; }
	while (s->allocations) {
		struct gma_sgx_device_allocation *a = s->allocations;

		s->allocations = a->next; a->next = f->device_allocations; f->device_allocations = a;
	}
	stage_drop_metadata(b);
	return 0;
}

static int stage_publish(struct gma_sgx_file *f, const struct sgx545_init2_in32 *wire,
	struct sgx545_init_stage *message, int (*publish)(void *, const struct sgx545_init_stage *), void *context)
{
	struct gma_sgx_stage *s;
	int ret = stage_prepare(f, wire, message, &s);

	if (ret) return ret;
	ret = stage_commit(f, s);
	if (ret) { kfree(s); return ret; }
	message->stage = s->id; message->buffers = s->count; message->pages = f->bridge->stats.staged_pages;
	ret = publish(context, message);
	if (ret) {
		if (stage_rollback(f)) return -EIO;
		return ret;
	}
	/* A delivered staging attempt ends ownership but never finalizes hardware. */
	backend_init_abort(f, 0);
	f->session.initializer = 0;
	return 0;
}

static int stage_abort(struct gma_sgx_bridge *b)
{
	struct gma_sgx_stage *s = b->stage;

	if (!s) return 0;
	while (s->allocations) {
		struct gma_sgx_device_allocation *a = s->allocations;
		u32 pages = a->binding.pages, before = b->stats.device_allocations;
		int ret = device_free_owned(b, s, &s->allocations);

		if (b->stats.device_allocations != before) {
			b->stats.staged_buffers--; b->stats.staged_pages -= pages;
		}
		if (ret) { s->dirty = true; b->stats.stage_errors++; return ret; }
	}
	stage_drop_metadata(b);
	return 0;
}

static int stage_control(struct gma_sgx_file *f, struct sgx545_init_control *message)
{
	struct gma_sgx_bridge *b = f->bridge;
	struct gma_sgx_stage *s = b->stage;
	u32 i;
	int ret;

	if (message->flags || message->operation > SGX545_STAGE_ABORT) return -EINVAL;
	if (!f->session.services || message->services != f->session.services || !s || message->stage != s->id)
		return -ENOENT;
	if (f->owner != s->owner || !OSProcHasPrivSrvInit()) return -EPERM;
	if (message->operation == SGX545_STAGE_ABORT) {
		ret = stage_abort(b);
		if (ret) return ret;
		message->buffers = message->pages = 0;
		return 0;
	}
	if (s->dirty) return -EIO;
	for (i = 0; i < s->count; i++) {
		struct gma_sgx_device_allocation *a = s->allocations;
		struct device_visit_context visit = {.verify = true};

		while (a && a->token != s->tokens[i]) a = a->next;
		if (!a) return -EIO;
		visit.allocation = a;
		ret = gma_sgx_buffer_visit(b->buffers, s, a->token, device_visit, &visit);
		if (ret) return ret;
	}
	ret = gma_sgx_init_check(&s->snapshot, s->memory, s->count);
	if (ret) return ret;
	message->buffers = b->stats.staged_buffers; message->pages = b->stats.staged_pages;
	return 0;
}

static int backend_disconnect(void *ctx)
{
	struct gma_sgx_file *f = ctx;
	int ret;

	if (f->bridge->init_owner == f)
		backend_init_abort(f, 0);
	while (f->device_allocations) {
		ret = backend_free_device(f, f->device_allocations->token);
		if (ret)
			return ret;
	}
	gma_sgx_buffers_release_owner(f->bridge->buffers, f);
	if (!f->handles)
		return 0;
	ret = PVRSRVFreeHandleBase(f->handles);
	if (ret) {
		f->bridge->stats.errors++;
		return ret;
	}
	f->handles = NULL;
	f->service_handle = f->device_handle = NULL;
	if (f->counted)
		f->bridge->stats.connections--;
	f->counted = false;
	f->bridge->stats.heap_handles -= f->heap_count;
	f->heap_count = 0;
	memset(f->heap_handles, 0, sizeof(f->heap_handles));
	memset(&f->init_info, 0, sizeof(f->init_info));
	f->info_ready = f->info_dirty = false;
	return 0;
}

static int backend_alloc_shared(void *ctx, u32 flags, u32 bytes, struct sgx545_alloc_shared_out *out)
{
	struct gma_sgx_file *f = ctx;
	u32 token;
	int ret;

	if (f->bridge->init_owner != f)
		return -EACCES;
	ret = new_token(f->bridge, &token);
	if (ret)
		return ret;
	return gma_sgx_buffer_alloc(f->bridge->buffers, f, token, flags, bytes, out);
}
static int backend_free_shared(void *ctx, u32 token)
{
	struct gma_sgx_file *f = ctx;

	if (*device_slot(f, token))
		return -EINVAL; /* A CPU-only free must never strand native PTEs. */
	return gma_sgx_buffer_free(f->bridge->buffers, f, token);
}
static int backend_map_info(void *ctx, u32 token, struct sgx545_map_info_out *out)
{
	struct gma_sgx_file *f = ctx;

	return gma_sgx_buffer_map_info(f->bridge->buffers, f, token, out);
}

static int bridge_copy_in(void *ctx, void *dst, u64 src, u32 size)
{
	return copy_from_user(dst, u64_to_user_ptr(src), size) ? -EFAULT : 0;
}
static int bridge_copy_out(void *ctx, u64 dst, const void *src, u32 size)
{
	return copy_to_user(u64_to_user_ptr(dst), src, size) ? -EFAULT : 0;
}

static const struct sgx545_bridge_ops bridge_ops = {
	.connect = backend_connect, .enumerate = backend_enumerate,
	.acquire = backend_acquire, .release_device = backend_release_device,
	.disconnect = backend_disconnect,
	.copy_in = bridge_copy_in, .copy_out = bridge_copy_out,
	.init_connect = backend_init_connect, .init_abort = backend_init_abort,
	.init_info = backend_init_info, .release_init_info = backend_release_init_info,
	.alloc_shared = backend_alloc_shared, .free_shared = backend_free_shared, .map_info = backend_map_info,
	.alloc_device = backend_alloc_device, .free_device = backend_free_device,
};

static void bridge_free(struct kref *ref)
{
	struct gma_sgx_bridge *b = container_of(ref, struct gma_sgx_bridge, ref);

	WARN_ON(!list_empty(&b->files));
	kfree(b);
}

static void bridge_file_free(struct gma_sgx_file *f)
{
	list_del(&f->link);
	put_pid(f->owner);
	kfree(f);
}

static int bridge_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct gma_sgx_bridge *b = container_of(misc, struct gma_sgx_bridge, misc);
	struct gma_sgx_file *f = kzalloc(sizeof(*f), GFP_KERNEL);

	if (!f)
		return -ENOMEM;
	/* misc_open serializes this callback against misc_deregister. */
	mutex_lock(&b->lock);
	if (!b->online) {
		mutex_unlock(&b->lock);
		kfree(f);
		return -ENODEV;
	}
	kref_get(&b->ref);
	f->bridge = b;
	f->owner = get_pid(task_tgid(current));
	list_add(&f->link, &b->files);
	b->stats.files++;
	mutex_unlock(&b->lock);
	file->private_data = f;
	return nonseekable_open(inode, file);
}

static int bridge_release(struct inode *inode, struct file *file)
{
	struct gma_sgx_file *f = file->private_data;
	struct gma_sgx_bridge *b = f->bridge;
	bool freed;

	mutex_lock(&b->lock);
	if (b->online && f->handles) {
		LinuxLockMutexNested(&gPVRSRVLock, PVRSRV_LOCK_CLASS_BRIDGE);
		backend_disconnect(f);
		LinuxUnLockMutex(&gPVRSRVLock);
	}
	b->stats.files--;
	/* Keep failed cleanup owned until bridge_stop can retry it. */
	f->closed = true;
	freed = !f->handles;
	if (freed)
		bridge_file_free(f);
	mutex_unlock(&b->lock);
	if (freed)
		kref_put(&b->ref, bridge_free);
	return 0;
}

struct mmu_visit_context {
	BM_HEAP *heap;
	struct sgx545_mmu_check *check;
};

static int bridge_mmu_visit(void *context, struct page **pages, u32 count)
{
	struct mmu_visit_context *ctx = context;
	u32 physical[4], i, j, swap;

	if (count != 4)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		if (upper_32_bits(page_to_phys(pages[i])))
			return -EOVERFLOW;
		physical[i] = page_to_phys(pages[i]);
	}
	/* Descending pages exercise scatter mapping even on contiguous host RAM. */
	for (i = 0; i < count; i++)
		for (j = i + 1; j < count; j++)
			if (physical[i] < physical[j]) {
				swap = physical[i]; physical[i] = physical[j]; physical[j] = swap;
			}
	return gma3600_sgx_mmu_exercise(ctx->heap->pMMUHeap, physical, count,
		ctx->check->flags, ctx->check);
}

static int bridge_mmu_check(struct gma_sgx_file *f, struct sgx545_mmu_check *check)
{
	struct mmu_visit_context context = {.check = check};
	u32 i;
	int ret;

	if (f->bridge->init_owner != f || !f->info_ready)
		return -EACCES;
	if (f->device_allocations || f->info_dirty)
		return -EBUSY;
	if (!f->session.services || check->services != f->session.services ||
	    !f->session.device || check->device != f->session.device)
		return -ENOENT;
	if (check->flags & ~SGX545_MMU_CROSS_BOUNDARY)
		return -EINVAL;
	for (i = 0; i < SGX545_MAX_HEAPS; i++)
		if (f->init_info.heaps[i].handle && f->init_info.heaps[i].handle == check->heap)
			break;
	if (i == SGX545_MAX_HEAPS || !f->heap_handles[i])
		return -ENOENT;
	ret = PVRSRVLookupHandle(f->handles, (void **)&context.heap, f->heap_handles[i],
		PVRSRV_HANDLE_TYPE_DEV_MEM_HEAP);
	if (ret)
		return -EIO;
	return gma_sgx_buffer_visit(f->bridge->buffers, f, check->buffer, bridge_mmu_visit, &context);
}

static int bridge_device_check(struct gma_sgx_file *f, struct sgx545_device_check *check)
{
	struct device_visit_context context = {.verify = true};
	int ret;

	if (!f->session.services || check->services != f->session.services ||
	    !f->session.device || check->device != f->session.device)
		return -ENOENT;
	context.allocation = *device_slot(f, check->buffer);
	if (!context.allocation)
		return -ENOENT;
	ret = gma_sgx_buffer_visit(f->bridge->buffers, f, check->buffer, device_visit, &context);
	if (!ret) {
		check->address = context.allocation->binding.address;
		check->pages = context.allocation->binding.pages;
		check->page_tables = context.tables;
	}
	return ret;
}

static int stage_copy_result(void *context, const struct sgx545_init_stage *message)
{
	return copy_to_user(context, message, sizeof(*message)) ? -EFAULT : 0;
}

static long bridge_init_ioctl(struct gma_sgx_file *f, unsigned int cmd, unsigned long arg)
{
	struct gma_sgx_bridge *b = f->bridge;
	struct sgx545_init2_in32 *wire = NULL;
	union {
		struct sgx545_init_requirements caps;
		struct sgx545_init_stage stage;
		struct sgx545_init_control control;
	} message;
	u32 size = cmd == SGX545_IOCTL_INIT_REQUIREMENTS ? sizeof(message.caps) :
		cmd == SGX545_IOCTL_INIT_STAGE ? sizeof(message.stage) : sizeof(message.control);
	int ret;

	if (copy_from_user(&message, (void __user *)arg, size)) return -EFAULT;
	if (cmd == SGX545_IOCTL_INIT_STAGE) {
		if (message.stage.request_bytes != sizeof(*wire)) return -EINVAL;
		wire = kzalloc(sizeof(*wire), GFP_KERNEL);
		if (!wire) return -ENOMEM;
		if (copy_from_user(wire, u64_to_user_ptr((u64)message.stage.request), sizeof(*wire))) {
			kfree(wire); return -EFAULT;
		}
	}
	mutex_lock(&b->lock);
	if (!b->online) { ret = -ENODEV; goto out; }
	LinuxLockMutexNested(&gPVRSRVLock, PVRSRV_LOCK_CLASS_BRIDGE);
	if (cmd == SGX545_IOCTL_INIT_REQUIREMENTS) ret = stage_requirements(f, &message.caps);
	else if (cmd == SGX545_IOCTL_INIT_STAGE)
		ret = stage_publish(f, wire, &message.stage, stage_copy_result, (void __user *)arg);
	else ret = stage_control(f, &message.control);
	/* STAGE copies its result transactionally. QUERY/ABORT own no new resource. */
	if (!ret && cmd != SGX545_IOCTL_INIT_STAGE && copy_to_user((void __user *)arg, &message, size)) ret = -EFAULT;
	LinuxUnLockMutex(&gPVRSRVLock);
out:
	mutex_unlock(&b->lock);
	kfree(wire);
	return ret;
}

static long bridge_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct gma_sgx_file *f = file->private_data;
	struct gma_sgx_bridge *b = f->bridge;
	struct sgx545_bridge32 wire;
	struct sgx545_bridge_request request;
	int ret;

	if (cmd != SGX545_IOCTL_SERVICES && cmd != SGX545_IOCTL_SERVICES_ALIAS &&
	    cmd != SGX545_IOCTL_BUFFER_CHECK && cmd != SGX545_IOCTL_MMU_CHECK && cmd != SGX545_IOCTL_DEVICE_CHECK && cmd != SGX545_IOCTL_INIT_REQUIREMENTS &&
	    cmd != SGX545_IOCTL_INIT_STAGE && cmd != SGX545_IOCTL_INIT_CONTROL)
		return -ENOTTY;
	if (task_tgid(current) != f->owner)
		return -EPERM;
	if (cmd == SGX545_IOCTL_INIT_REQUIREMENTS || cmd == SGX545_IOCTL_INIT_STAGE || cmd == SGX545_IOCTL_INIT_CONTROL)
		return bridge_init_ioctl(f, cmd, arg);
	if (cmd == SGX545_IOCTL_DEVICE_CHECK) {
		struct sgx545_device_check check;

		if (copy_from_user(&check, (void __user *)arg, sizeof(check)))
			return -EFAULT;
		mutex_lock(&b->lock);
		if (!b->online) ret = -ENODEV;
		else {
			LinuxLockMutexNested(&gPVRSRVLock, PVRSRV_LOCK_CLASS_BRIDGE);
			ret = bridge_device_check(f, &check);
			LinuxUnLockMutex(&gPVRSRVLock);
		}
		mutex_unlock(&b->lock);
		if (!ret && copy_to_user((void __user *)arg, &check, sizeof(check)))
			return -EFAULT;
		return ret;
	}
	if (cmd == SGX545_IOCTL_MMU_CHECK) {
		struct sgx545_mmu_check check;

		if (copy_from_user(&check, (void __user *)arg, sizeof(check)))
			return -EFAULT;
		mutex_lock(&b->lock);
		if (!b->online) ret = -ENODEV;
		else {
			LinuxLockMutexNested(&gPVRSRVLock, PVRSRV_LOCK_CLASS_BRIDGE);
			ret = bridge_mmu_check(f, &check);
			LinuxUnLockMutex(&gPVRSRVLock);
		}
		mutex_unlock(&b->lock);
		if (!ret && copy_to_user((void __user *)arg, &check, sizeof(check)))
			return -EFAULT;
		return ret;
	}
	if (cmd == SGX545_IOCTL_BUFFER_CHECK) {
		struct sgx545_buffer_check check;

		if (copy_from_user(&check, (void __user *)arg, sizeof(check)))
			return -EFAULT;
		mutex_lock(&b->lock);
		if (!b->online) ret = -ENODEV;
		else if (!f->session.services || check.services != f->session.services) ret = -ENOENT;
		else if (b->init_owner != f) ret = -EACCES;
		else ret = gma_sgx_buffer_check(b->buffers, f, check.handle, check.expected, check.fill);
		mutex_unlock(&b->lock);
		return ret;
	}
	if (copy_from_user(&wire, (void __user *)arg, sizeof(wire)))
		return -EFAULT;
	ret = sgx545_bridge32_decode(&wire, sizeof(wire), &request);
	if (ret)
		return ret;
	mutex_lock(&b->lock);
	if (!b->online) {
		ret = -ENODEV;
	} else {
		LinuxLockMutexNested(&gPVRSRVLock, PVRSRV_LOCK_CLASS_BRIDGE);
		ret = sgx545_bridge_execute(&f->session, &bridge_ops, f, &request);
		LinuxUnLockMutex(&gPVRSRVLock);
	}
	mutex_unlock(&b->lock);
	return ret;
}

static int bridge_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct gma_sgx_file *f = file->private_data;
	struct gma_sgx_bridge *b = f->bridge;
	int ret;

	if (task_tgid(current) != f->owner)
		return -EPERM;
	mutex_lock(&b->lock);
	if (!b->online) ret = -ENODEV;
	else ret = gma_sgx_buffer_mmap(b->buffers, f, vma);
	mutex_unlock(&b->lock);
	return ret;
}

#ifdef CONFIG_COMPAT
static long bridge_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return bridge_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif
static const struct file_operations bridge_fops = {
	.owner = THIS_MODULE, .open = bridge_open, .release = bridge_release,
	.unlocked_ioctl = bridge_ioctl,
	.mmap = bridge_mmap,
#ifdef CONFIG_COMPAT
	.compat_ioctl = bridge_compat_ioctl,
#endif
};

static void bridge_stop(void *data)
{
	struct gma_sgx_bridge *b = data;
	struct gma_sgx_file *f, *next;

	/* Do not hold b->lock while taking the misc core's open/deregister lock. */
	if (b->registered)
		misc_deregister(&b->misc);
	mutex_lock(&b->lock);
	b->online = false;
	LinuxLockMutexNested(&gPVRSRVLock, PVRSRV_LOCK_CLASS_BRIDGE);
	list_for_each_entry_safe(f, next, &b->files, link) {
		backend_disconnect(f);
		if (f->handles) {
			/* Retain failed CPU allocations; do not free their owner record. */
			pr_err("gma3600 SGX bridge: retaining failed handle-base cleanup\n");
			continue;
		}
		/* No late file close may call the DDK after common services teardown. */
		memset(&f->session, 0, sizeof(f->session));
		if (f->closed) {
			bridge_file_free(f);
			kref_put(&b->ref, bridge_free);
		}
	}
	stage_abort(b); /* Native PTEs disappear before common DDK teardown. */
	LinuxUnLockMutex(&gPVRSRVLock);
	gma_sgx_buffers_stop(b->buffers);
	pr_info("gma3600 SGX bridge stopped: files=%u connections=%u errors=%u heap_handles=%u init_running=%u init_ran=%u device_allocations=%u device_pages=%u device_errors=%u stages=%u staged_buffers=%u staged_pages=%u stage_errors=%u\n",
		b->stats.files, b->stats.connections, b->stats.errors,
		b->stats.heap_handles, b->stats.init_running, b->stats.init_ran,
		b->stats.device_allocations, b->stats.device_pages, b->stats.device_errors,
		b->stats.stages, b->stats.staged_buffers, b->stats.staged_pages, b->stats.stage_errors);
	mutex_unlock(&b->lock);
	kref_put(&b->ref, bridge_free);
}

int gma3600_sgx_bridge_register(struct device *dev, struct gma_sgx_bridge **result)
{
	struct gma_sgx_bridge *b = kzalloc(sizeof(*b), GFP_KERNEL);
	int ret;

	if (!b)
		return -ENOMEM;
	kref_init(&b->ref);
	mutex_init(&b->lock);
	INIT_LIST_HEAD(&b->files);
	b->buffers = gma_sgx_buffers_create();
	if (!b->buffers) {
		kfree(b);
		return -ENOMEM;
	}
	b->misc.minor = MISC_DYNAMIC_MINOR;
	b->misc.name = "gma3600-sgx-bridge";
	b->misc.mode = 0600;
	b->misc.parent = dev;
	b->misc.fops = &bridge_fops;
	ret = devm_add_action_or_reset(dev, bridge_stop, b);
	if (ret)
		return ret;
	b->online = true;
	ret = misc_register(&b->misc);
	if (ret)
		return ret;
	b->registered = true;
	*result = b;
	return 0;
}

void gma3600_sgx_bridge_stats(struct gma_sgx_bridge *b, struct gma_sgx_bridge_stats *stats)
{
	struct gma_sgx_buffer_stats buffers;

	mutex_lock(&b->lock);
	*stats = b->stats;
	gma_sgx_buffers_stats(b->buffers, &buffers);
	stats->buffers = buffers.buffers;
	stats->buffer_pages = buffers.pages;
	stats->buffer_maps = buffers.maps;
	stats->buffer_errors = buffers.errors;
	mutex_unlock(&b->lock);
}
