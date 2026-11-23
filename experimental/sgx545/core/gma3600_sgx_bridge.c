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

#if !defined(PVR_SECURE_HANDLES)
#error The diagnostic bridge requires DDK secure handles
#endif
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

struct gma_sgx_bridge {
	struct miscdevice misc;
	struct kref ref;
	struct mutex lock;
	struct list_head files;
	struct gma_sgx_bridge_stats stats;
	struct gma_sgx_file *init_owner;
	u32 next_token;
	bool online, registered;
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
};

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

static int backend_disconnect(void *ctx)
{
	struct gma_sgx_file *f = ctx;
	int ret;

	if (f->bridge->init_owner == f)
		backend_init_abort(f, 0);
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

static long bridge_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct gma_sgx_file *f = file->private_data;
	struct gma_sgx_bridge *b = f->bridge;
	struct sgx545_bridge32 wire;
	struct sgx545_bridge_request request;
	int ret;

	if (cmd != SGX545_IOCTL_SERVICES && cmd != SGX545_IOCTL_SERVICES_ALIAS)
		return -ENOTTY;
	if (task_tgid(current) != f->owner)
		return -EPERM;
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

#ifdef CONFIG_COMPAT
static long bridge_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return bridge_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif
static const struct file_operations bridge_fops = {
	.owner = THIS_MODULE, .open = bridge_open, .release = bridge_release,
	.unlocked_ioctl = bridge_ioctl,
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
	LinuxUnLockMutex(&gPVRSRVLock);
	pr_info("gma3600 SGX bridge stopped: files=%u connections=%u errors=%u heap_handles=%u init_running=%u init_ran=%u\n",
		b->stats.files, b->stats.connections, b->stats.errors,
		b->stats.heap_handles, b->stats.init_running, b->stats.init_ran);
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
	mutex_lock(&b->lock);
	*stats = b->stats;
	mutex_unlock(&b->lock);
}
