// SPDX-License-Identifier: GPL-2.0-only
/* File-owned CPU buffers; no DDK cache, GPU mapping or hardware access.
 * VMAs retain independent references so auxiliary detach may tear down DDK
 * services without freeing mapped RAM or invalidating its WC cache aliases.
 */
#include <linux/err.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include "sgx545_abi.h"
#include "gma3600_sgx_memory.h"
#include "gma3600_sgx_buffers.h"

#define BUFFER_MAX_COUNT 64U
#define BUFFER_MAX_PAGES 4096U
#define BUFFER_OWNER_MAX 32U

struct gma_sgx_buffers {
	struct kref ref;
	struct mutex lock;
	struct list_head buffers;
	struct gma_sgx_buffer_stats stats;
	bool online;
};
struct gma_sgx_buffer {
	struct list_head link;
	struct gma_sgx_buffers *pool;
	struct gma3600_sgx_pages *allocation;
	void *owner, *linear;
	u32 token, bytes, size, refs;
};

static void pool_destroy(struct kref *ref)
{
	struct gma_sgx_buffers *p = container_of(ref, struct gma_sgx_buffers, ref);

	WARN_ON(!list_empty(&p->buffers));
	kfree(p);
}

/* All buffer refs, ownership and counters are protected by pool->lock. */
static void buffer_put_locked(struct gma_sgx_buffer *b)
{
	struct gma_sgx_buffers *p = b->pool;
	u32 pages;

	if (WARN_ON(!b->refs) || --b->refs)
		return;
	pages = b->allocation->count;
	if (b->linear) {
		vunmap(b->linear);
		b->linear = NULL;
	}
	if (gma3600_sgx_pages_free(b->allocation)) {
		/* Preserve metadata/pages if safe WB restoration failed. */
		p->stats.errors++;
		pr_err("gma3600 SGX buffers: retaining pages after WB restoration failure\n");
		return;
	}
	list_del(&b->link);
	p->stats.pages -= pages;
	p->stats.buffers--;
	kfree(b);
	/* Caller holds either the base reference or a temporary pool reference. */
	kref_put(&p->ref, pool_destroy);
}

static struct gma_sgx_buffer *buffer_find(struct gma_sgx_buffers *p, void *owner, u32 token)
{
	struct gma_sgx_buffer *b;

	list_for_each_entry(b, &p->buffers, link)
		if (b->owner == owner && b->token == token)
			return b;
	return NULL;
}

struct gma_sgx_buffers *gma_sgx_buffers_create(void)
{
	struct gma_sgx_buffers *p = kzalloc(sizeof(*p), GFP_KERNEL);

	if (!p)
		return NULL;
	kref_init(&p->ref);
	mutex_init(&p->lock);
	INIT_LIST_HEAD(&p->buffers);
	p->online = true;
	return p;
}

int gma_sgx_buffer_alloc(struct gma_sgx_buffers *p, void *owner, u32 token,
	u32 flags, u32 bytes, struct sgx545_alloc_shared_out *out)
{
	struct gma_sgx_buffer *b, *it;
	u32 count, owned = 0;
	int ret;

	if (!owner || !token || !bytes || bytes > SGX545_BUFFER_MAX_BYTES)
		return -EINVAL;
	if (flags != SGX545_BUFFER_WC)
		return -EOPNOTSUPP;
	count = DIV_ROUND_UP(bytes, PAGE_SIZE);
	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	mutex_lock(&p->lock);
	if (!p->online) { ret = -ENODEV; goto fail; }
	list_for_each_entry(it, &p->buffers, link) {
		if (it->token == token) { ret = -EEXIST; goto fail; }
		owned += it->owner == owner;
	}
	if (owned >= BUFFER_OWNER_MAX || p->stats.buffers >= BUFFER_MAX_COUNT ||
	    count > BUFFER_MAX_PAGES - p->stats.pages) {
		ret = -ENOSPC;
		goto fail;
	}
	b->pool = p;
	b->token = token;
	b->bytes = bytes;
	b->size = count * PAGE_SIZE;
	ret = gma3600_sgx_pages_alloc(count, GMA_SGX_WC, &b->allocation);
	if (ret && !b->allocation)
		goto fail;
	/* Allocation may return owned pages on a failed cache rollback. Track them. */
	kref_get(&p->ref);
	list_add(&b->link, &p->buffers);
	p->stats.buffers++;
	p->stats.pages += count;
	b->refs = 1;
	if (ret)
		goto rollback;
	b->linear = vmap(b->allocation->pages, count, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
	if (!b->linear) {
		ret = -ENOMEM;
		goto rollback;
	}
	b->owner = owner;
	memset(out, 0, sizeof(*out));
	out->kernel_mem = token;
	out->mem.flags = SGX545_BUFFER_WC | SGX545_BUFFER_MULTI;
	out->mem.alloc_size = bytes;
	out->mem.mapping = out->mem.kernel_mem = token;
	mutex_unlock(&p->lock);
	return 0;
rollback:
	buffer_put_locked(b);
	mutex_unlock(&p->lock);
	return ret;
fail:
	mutex_unlock(&p->lock);
	kfree(b);
	return ret;
}

int gma_sgx_buffer_free(struct gma_sgx_buffers *p, void *owner, u32 token)
{
	struct gma_sgx_buffer *b;
	u32 errors;
	int ret = 0;

	mutex_lock(&p->lock);
	b = buffer_find(p, owner, token);
	if (!p->online) ret = -ENODEV;
	else if (!b) ret = -ENOENT;
	else {
		errors = p->stats.errors;
		b->owner = NULL;
		buffer_put_locked(b);
		if (p->stats.errors != errors)
			ret = -EIO;
	}
	mutex_unlock(&p->lock);
	return ret;
}

void gma_sgx_buffers_release_owner(struct gma_sgx_buffers *p, void *owner)
{
	struct gma_sgx_buffer *b, *next;

	mutex_lock(&p->lock);
	list_for_each_entry_safe(b, next, &p->buffers, link) {
		if (b->owner == owner) {
			b->owner = NULL;
			buffer_put_locked(b);
		}
	}
	mutex_unlock(&p->lock);
}

int gma_sgx_buffer_map_info(struct gma_sgx_buffers *p, void *owner, u32 token,
	struct sgx545_map_info_out *out)
{
	struct gma_sgx_buffer *b;
	int ret = 0;

	mutex_lock(&p->lock);
	b = buffer_find(p, owner, token);
	if (!p->online) ret = -ENODEV;
	else if (!b) ret = -ENOENT;
	else {
		memset(out, 0, sizeof(*out));
		out->offset = token; /* mmap2 page units, never a PFN. */
		out->real_size = b->size;
	}
	mutex_unlock(&p->lock);
	return ret;
}

int gma_sgx_buffer_measure(struct gma_sgx_buffers *p, void *owner, u32 token,
	struct sgx545_content_measure *out)
{
	struct gma_sgx_buffer *b;
	int ret = 0;

	memset(out, 0, sizeof(*out));
	mutex_lock(&p->lock);
	b = buffer_find(p, owner, token);
	if (!p->online) ret = -ENODEV;
	else if (!b || !b->linear) ret = -ENOENT;
	else {
		/* Complete CPU WC writes before reading through the kernel alias.
		 * This does not synchronize an active GPU or freeze other CPU writers. */
		mb();
		*out = sgx545_measure_content(b->linear, b->bytes, b->size);
	}
	mutex_unlock(&p->lock);
	return ret;
}

static void buffer_vma_open(struct vm_area_struct *vma)
{
	struct gma_sgx_buffer *b = vma->vm_private_data;

	mutex_lock(&b->pool->lock);
	b->refs++;
	b->pool->stats.maps++;
	mutex_unlock(&b->pool->lock);
}

static void buffer_vma_close(struct vm_area_struct *vma)
{
	struct gma_sgx_buffer *b = vma->vm_private_data;
	struct gma_sgx_buffers *p = b->pool;

	kref_get(&p->ref); /* Last VMA may also own the last buffer/pool reference. */
	mutex_lock(&p->lock);
	p->stats.maps--;
	buffer_put_locked(b);
	mutex_unlock(&p->lock);
	kref_put(&p->ref, pool_destroy);
}

static vm_fault_t buffer_vma_fault(struct vm_fault *vmf)
{
	struct gma_sgx_buffer *b = vmf->vma->vm_private_data;
	unsigned long index;

	if (vmf->pgoff < b->token)
		return VM_FAULT_SIGBUS;
	index = vmf->pgoff - b->token; /* Remains correct after mprotect/VMA splits. */
	if (index >= b->allocation->count)
		return VM_FAULT_SIGBUS;
	return vmf_insert_page(vmf->vma, vmf->address, b->allocation->pages[index]);
}

static const struct vm_operations_struct buffer_vm_ops = {
	.open = buffer_vma_open, .close = buffer_vma_close, .fault = buffer_vma_fault,
};

int gma_sgx_buffer_mmap(struct gma_sgx_buffers *p, void *owner, struct vm_area_struct *vma)
{
	struct gma_sgx_buffer *b;
	int ret = 0;

	if (!(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC))
		return -EACCES;
	if (vma->vm_pgoff > U32_MAX)
		return -EINVAL;
	mutex_lock(&p->lock);
	b = buffer_find(p, owner, vma->vm_pgoff);
	if (!p->online) ret = -ENODEV;
	else if (!b) ret = -ENOENT;
	else if (vma->vm_end - vma->vm_start != b->size) ret = -EINVAL;
	else {
		vm_flags_set(vma, VM_MIXEDMAP | VM_DONTCOPY | VM_DONTEXPAND | VM_DONTDUMP);
		vm_flags_clear(vma, VM_MAYEXEC);
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
		vma->vm_private_data = b;
		vma->vm_ops = &buffer_vm_ops;
		b->refs++;
		p->stats.maps++;
	}
	mutex_unlock(&p->lock);
	return ret;
}

int gma_sgx_buffer_check(struct gma_sgx_buffers *p, void *owner, u32 token, u32 expected, u32 fill)
{
	struct gma_sgx_buffer *b;
	volatile u32 *words;
	u32 i;
	int ret = 0;

	mutex_lock(&p->lock);
	b = buffer_find(p, owner, token);
	if (!p->online) ret = -ENODEV;
	else if (!b) ret = -ENOENT;
	else {
		words = b->linear;
		rmb();
		for (i = 0; i < b->size / sizeof(u32); i++)
			if (words[i] != (expected ^ i)) { ret = -EIO; break; }
		if (!ret) {
			for (i = 0; i < b->size / sizeof(u32); i++)
				words[i] = fill ^ i;
			wmb();
		}
	}
	mutex_unlock(&p->lock);
	return ret;
}

void gma_sgx_buffers_stats(struct gma_sgx_buffers *p, struct gma_sgx_buffer_stats *out)
{
	mutex_lock(&p->lock);
	*out = p->stats;
	mutex_unlock(&p->lock);
}

int gma_sgx_buffer_visit(struct gma_sgx_buffers *p, void *owner, u32 token,
	int (*visit)(void *, struct page **, u32), void *context)
{
	struct gma_sgx_buffer *b;
	int ret;

	mutex_lock(&p->lock);
	b = buffer_find(p, owner, token);
	if (!p->online) ret = -ENODEV;
	else if (!b) ret = -ENOENT;
	else ret = visit(context, b->allocation->pages, b->allocation->count);
	mutex_unlock(&p->lock);
	return ret;
}

int gma_sgx_buffer_pin(struct gma_sgx_buffers *p, void *owner, u32 token, void **pin)
{
	struct gma_sgx_buffer *b;
	int ret = 0;

	*pin = NULL;
	mutex_lock(&p->lock);
	b = buffer_find(p, owner, token);
	if (!p->online) ret = -ENODEV;
	else if (!b) ret = -ENOENT;
	else { b->refs++; *pin = b; }
	mutex_unlock(&p->lock);
	return ret;
}

void gma_sgx_buffer_unpin(void *pin)
{
	struct gma_sgx_buffer *b = pin;
	struct gma_sgx_buffers *p = b->pool;

	kref_get(&p->ref);
	mutex_lock(&p->lock);
	buffer_put_locked(b);
	mutex_unlock(&p->lock);
	kref_put(&p->ref, pool_destroy);
}

int gma_sgx_buffer_describe(struct gma_sgx_buffers *p, void *owner, u32 token,
	void **linear, u32 *bytes)
{
	struct gma_sgx_buffer *b;
	int ret = 0;

	*linear = NULL; *bytes = 0;
	mutex_lock(&p->lock);
	b = buffer_find(p, owner, token);
	if (!p->online) ret = -ENODEV;
	else if (!b) ret = -ENOENT;
	else { *linear = b->linear; *bytes = b->bytes; }
	mutex_unlock(&p->lock);
	return ret;
}

int gma_sgx_buffers_transfer(struct gma_sgx_buffers *p, void *from, void *to,
	const u32 *tokens, u32 count)
{
	struct gma_sgx_buffer *b;
	u32 i, j, owned = 0;
	int ret = 0;

	if (!from || !to || from == to || !tokens || !count || count > BUFFER_OWNER_MAX)
		return -EINVAL;
	mutex_lock(&p->lock);
	if (!p->online) { ret = -ENODEV; goto out; }
	list_for_each_entry(b, &p->buffers, link) owned += b->owner == to;
	if (count > BUFFER_OWNER_MAX - owned) { ret = -ENOSPC; goto out; }
	/* Complete validation before changing even one owner. */
	for (i = 0; i < count; i++) {
		if (!buffer_find(p, from, tokens[i])) { ret = -ENOENT; goto out; }
		for (j = 0; j < i; j++)
			if (tokens[j] == tokens[i]) { ret = -EINVAL; goto out; }
	}
	for (i = 0; i < count; i++) buffer_find(p, from, tokens[i])->owner = to;
out:
	mutex_unlock(&p->lock);
	return ret;
}

void gma_sgx_buffers_stop(struct gma_sgx_buffers *p)
{
	struct gma_sgx_buffer *b, *next;

	mutex_lock(&p->lock);
	p->online = false;
	list_for_each_entry_safe(b, next, &p->buffers, link) {
		if (b->owner) {
			b->owner = NULL;
			buffer_put_locked(b);
		}
	}
	pr_info("gma3600 SGX buffers stopped: buffers=%u pages=%u maps=%u errors=%u (live VMAs defer release)\n",
		p->stats.buffers, p->stats.pages, p->stats.maps, p->stats.errors);
	mutex_unlock(&p->lock);
	kref_put(&p->ref, pool_destroy);
}
