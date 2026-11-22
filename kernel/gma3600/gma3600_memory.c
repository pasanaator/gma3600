// SPDX-License-Identifier: GPL-2.0-only
/* Cedarview display GTT pages, matching gma500 GEM's WC/physical-page path. */
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <asm/set_memory.h>

#include "gma3600_drv.h"

void gma3600_gem_free_pages(struct gma3600_gem_object *obj)
{
	int i;

	/* Callers unmap CPU aliases and remove the GTT binding first. */
	if (obj->pages_wc) {
		WARN_ON_ONCE(set_pages_array_wb(obj->pages, obj->num_pages));
		obj->pages_wc = false;
	}
	if (obj->pages) {
		for (i = 0; i < obj->num_pages; i++)
			if (obj->pages[i])
				__free_page(obj->pages[i]);
		kvfree(obj->pages);
		obj->pages = NULL;
	}
	kvfree(obj->phys_addrs);
	obj->phys_addrs = NULL;
}

int gma3600_gem_alloc_pages(struct gma3600_gem_object *obj)
{
	int i, ret;

	if (obj->num_pages <= 0)
		return -EINVAL;
	obj->pages = kvmalloc_array(obj->num_pages, sizeof(*obj->pages),
				    GFP_KERNEL | __GFP_ZERO);
	obj->phys_addrs = kvmalloc_array(obj->num_pages, sizeof(*obj->phys_addrs),
					 GFP_KERNEL | __GFP_ZERO);
	if (!obj->pages || !obj->phys_addrs) {
		ret = -ENOMEM;
		goto err_free;
	}
	for (i = 0; i < obj->num_pages; i++) {
		/* GTT PTEs take physical addresses, not DMA-remapped IOVAs.
		 * Use low, directly mapped RAM, including on 32-bit Atom kernels.
		 */
		obj->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO | __GFP_DMA32);
		if (!obj->pages[i]) {
			ret = -ENOMEM;
			goto err_free;
		}
		obj->phys_addrs[i] = page_to_phys(obj->pages[i]);
		if (upper_32_bits(obj->phys_addrs[i])) {
			ret = -EOVERFLOW;
			goto err_free;
		}
	}

	/* Change the direct-map alias too; mmap/vmap use WC consistently. */
	ret = set_pages_array_wc(obj->pages, obj->num_pages);
	if (ret) {
		WARN_ON_ONCE(set_pages_array_wb(obj->pages, obj->num_pages));
		goto err_free;
	}
	obj->pages_wc = true;
	return 0;

err_free:
	gma3600_gem_free_pages(obj);
	return ret;
}
