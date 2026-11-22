// SPDX-License-Identifier: GPL-2.0-only
/*
 * Intel GMA 3600 (Cedar Trail) DRM/KMS Driver — GEM & Dumb Buffers
 *
 * Copyright (C) 2026 Antigravity Project
 */

#include <linux/mm.h>
#include <linux/iosys-map.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_prime.h>

#include "gma3600_drv.h"

/*
 * GEM vm_operations: fault handler inserts backing pages on demand.
 * Uses VM_MIXEDMAP so that vm_insert_page() works correctly.
 */
static vm_fault_t gma3600_gem_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct drm_gem_object *gem_obj = vma->vm_private_data;
	struct gma3600_gem_object *obj = to_gma3600_gem(gem_obj);
	pgoff_t page_offset;
	int ret;

	page_offset = (vmf->address - vma->vm_start) >> PAGE_SHIFT;
	if (page_offset >= obj->num_pages)
		return VM_FAULT_SIGBUS;

	if (!obj->pages[page_offset])
		return VM_FAULT_SIGBUS;

	ret = vm_insert_page(vma, vmf->address, obj->pages[page_offset]);
	switch (ret) {
	case 0:
	case -EBUSY:
		return VM_FAULT_NOPAGE;
	case -ENOMEM:
		return VM_FAULT_OOM;
	default:
		return VM_FAULT_SIGBUS;
	}
}

static const struct vm_operations_struct gma3600_gem_vm_ops = {
	.fault = gma3600_gem_fault,
	.open  = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static int gma3600_gem_mmap(struct drm_gem_object *gem_obj,
			    struct vm_area_struct *vma)
{
	vma->vm_ops = &gma3600_gem_vm_ops;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
	vm_flags_clear(vma, VM_PFNMAP);
	vm_flags_set(vma, VM_IO | VM_MIXEDMAP | VM_DONTEXPAND | VM_DONTDUMP);
#else
	vma->vm_flags &= ~VM_PFNMAP;
	vma->vm_flags |= VM_IO | VM_MIXEDMAP | VM_DONTEXPAND | VM_DONTDUMP;
#endif
	vma->vm_page_prot = pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
	return 0;
}

void gma3600_gem_free_object(struct drm_gem_object *gem_obj)
{
	struct gma3600_gem_object *obj = to_gma3600_gem(gem_obj);
	struct gma3600_device *gdev = to_gma3600(gem_obj->dev);

	if (obj->vaddr) {
		vunmap(obj->vaddr);
		obj->vaddr = NULL;
	}

	if (obj->in_gtt)
		gma3600_gtt_remove(gdev, obj);

	gma3600_gem_free_pages(obj);

	drm_gem_object_release(&obj->base);
	kfree(obj);
}

static int gma3600_gem_vmap(struct drm_gem_object *gem, struct iosys_map *map)
{
	struct gma3600_gem_object *obj = to_gma3600_gem(gem);

	if (!obj->vaddr)
		obj->vaddr = vmap(obj->pages, obj->num_pages, VM_MAP,
				  pgprot_writecombine(PAGE_KERNEL));
	if (!obj->vaddr)
		return -ENOMEM;
	iosys_map_set_vaddr(map, obj->vaddr);
	return 0;
}

static void gma3600_gem_vunmap(struct drm_gem_object *gem, struct iosys_map *map)
{
	/* Keep the mapping until object destruction; concurrent clients share it. */
}

static const struct drm_gem_object_funcs gma3600_gem_funcs = {
	.free = gma3600_gem_free_object,
	.vmap = gma3600_gem_vmap,
	.vunmap = gma3600_gem_vunmap,
	.mmap = gma3600_gem_mmap,
};

struct gma3600_gem_object *gma3600_gem_create(struct gma3600_device *gdev,
					      size_t size)
{
	struct gma3600_gem_object *obj;
	int ret;

	size = PAGE_ALIGN(size);
	if (!size)
		return ERR_PTR(-EINVAL);

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	obj->num_pages = size >> PAGE_SHIFT;
	obj->gtt_num_pages = obj->num_pages;

	ret = drm_gem_object_init(&gdev->drm, &obj->base, size);
	if (ret) {
		kfree(obj);
		return ERR_PTR(ret);
	}

	obj->base.funcs = &gma3600_gem_funcs;

	ret = drm_gem_create_mmap_offset(&obj->base);
	if (ret)
		goto err_free_gem;

	ret = gma3600_gem_alloc_pages(obj);
	if (ret)
		goto err_put;

	ret = gma3600_gtt_insert(gdev, obj);
	if (ret)
		goto err_put;

	return obj;

err_put:
	drm_gem_object_put(&obj->base);
	return ERR_PTR(ret);
err_free_gem:
	drm_gem_object_release(&obj->base);
	kfree(obj);
	return ERR_PTR(ret);
}

int gma3600_gem_init(struct gma3600_device *gdev)
{
	return 0;
}

int gma3600_gem_dumb_create(struct drm_file *file, struct drm_device *dev,
			    struct drm_mode_create_dumb *args)
{
	struct gma3600_device *gdev = to_gma3600(dev);
	struct gma3600_gem_object *obj;
	u32 pitch;
	u64 size, row;
	int ret;

	/* 64-byte aligned stride is required by Intel display engines */
	if (!args->width || !args->height ||
	    (args->bpp != 16 && args->bpp != 32))
		return -EINVAL;
	row = ALIGN((u64)args->width * (args->bpp / 8), 64);
	size = row * args->height;
	if (row > U32_MAX || size > SIZE_MAX - PAGE_SIZE ||
	    size > gdev->aperture_size)
		return -EINVAL;
	pitch = row;

	args->pitch = pitch;
	args->size = size;

	obj = gma3600_gem_create(gdev, size);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	ret = drm_gem_handle_create(file, &obj->base, &args->handle);
	drm_gem_object_put(&obj->base);

	return ret;
}

int gma3600_gem_mmap_offset(struct drm_file *file, struct drm_device *dev,
			    u32 handle, u64 *offset)
{
	struct drm_gem_object *gem_obj;
	int ret;

	gem_obj = drm_gem_object_lookup(file, handle);
	if (!gem_obj)
		return -ENOENT;

	ret = drm_gem_create_mmap_offset(gem_obj);
	if (ret == 0)
		*offset = drm_vma_node_offset_addr(&gem_obj->vma_node);

	drm_gem_object_put(gem_obj);
	return ret;
}
