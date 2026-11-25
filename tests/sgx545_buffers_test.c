// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#include "sgx545_abi.h"
#include "gma3600_sgx_memory.h"
#include "gma3600_sgx_buffers.h"

struct list_head { struct list_head *next, *prev; };
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
#define INIT_LIST_HEAD(h) ((h)->next = (h)->prev = (h))
static bool list_empty(struct list_head *h) { return h->next == h; }
static void list_add(struct list_head *n, struct list_head *h)
{ n->next = h->next; n->prev = h; h->next->prev = n; h->next = n; }
static void list_del(struct list_head *n)
{ n->prev->next = n->next; n->next->prev = n->prev; }
#define list_for_each_entry(pos, head, member) \
    for (struct list_head *cursor_ = (head)->next; cursor_ != (head) && \
         ((pos) = container_of(cursor_, __typeof__(*(pos)), member), 1); cursor_ = cursor_->next)
#define list_for_each_entry_safe(pos, next_pos, head, member) \
    for (struct list_head *it = (head)->next, *nt = it->next; \
         it != (head) && ((pos) = container_of(it, __typeof__(*(pos)), member), \
         (next_pos) = nt == (head) ? NULL : container_of(nt, __typeof__(*(pos)), member), \
         (void)(next_pos), 1); it = nt, nt = it->next)
struct mutex { int locked; };
static void mutex_init(struct mutex *m) { m->locked = 0; }
static void mutex_lock(struct mutex *m) { assert(!m->locked); m->locked = 1; }
static void mutex_unlock(struct mutex *m) { assert(m->locked); m->locked = 0; }
struct kref { unsigned refs; };
static void kref_init(struct kref *r) { r->refs = 1; }
static void kref_get(struct kref *r) { assert(r->refs); r->refs++; }
static void kref_put(struct kref *r, void (*free_)(struct kref *))
{ assert(r->refs); if (!--r->refs) free_(r); }
static unsigned metadata, pages_live, mappings, fail_alloc, fail_vmap, fail_wb, fail_pages;
static void *kzalloc(size_t size, int flags)
{
    void *p;
    if (fail_alloc) { fail_alloc--; return NULL; }
    p = calloc(1, size); assert(p); metadata++; return p;
}
static void kfree(void *p) { assert(p && metadata); metadata--; free(p); }
#define GFP_KERNEL 0
#define U32_MAX UINT32_MAX
#define PAGE_SIZE 4096U
#define PAGE_KERNEL 0x100U
#define VM_MAP 1
#define VM_SHARED 1
#define VM_EXEC 2
#define VM_MAYEXEC 4
#define VM_MIXEDMAP 8
#define VM_DONTCOPY 16
#define VM_DONTEXPAND 32
#define VM_DONTDUMP 64
#define VM_FAULT_SIGBUS 1U
#define VM_FAULT_NOPAGE 2U
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define WARN_ON(value) ({ bool warn_ = !!(value); assert(!warn_); warn_; })
#define pr_err(...) ((void)0)
#define pr_info(...) ((void)0)
#define rmb() ((void)0)
#define wmb() ((void)0)
#define mb() ((void)0)
typedef unsigned vm_fault_t;
struct vm_operations_struct;
struct vm_area_struct {
    unsigned long vm_start, vm_end, vm_pgoff, vm_flags, vm_page_prot;
    void *vm_private_data;
    const struct vm_operations_struct *vm_ops;
};
struct vm_fault { struct vm_area_struct *vma; unsigned long address, pgoff; };
struct vm_operations_struct {
    void (*open)(struct vm_area_struct *);
    void (*close)(struct vm_area_struct *);
    vm_fault_t (*fault)(struct vm_fault *);
};
static void vm_flags_set(struct vm_area_struct *v, unsigned long bits) { v->vm_flags |= bits; }
static void vm_flags_clear(struct vm_area_struct *v, unsigned long bits) { v->vm_flags &= ~bits; }
static unsigned long pgprot_writecombine(unsigned long prot) { return prot | 0x40; }
struct page { unsigned char *data; };
static struct page *inserted;
static vm_fault_t vmf_insert_page(struct vm_area_struct *v, unsigned long addr, struct page *page)
{ assert(page && addr >= v->vm_start && addr < v->vm_end); inserted = page; return VM_FAULT_NOPAGE; }
int gma3600_sgx_pages_alloc(unsigned count, enum gma3600_sgx_cache cache,
                          struct gma3600_sgx_pages **result)
{
    struct gma3600_sgx_pages *a;
    unsigned char *data;
    unsigned i;
    assert(cache == GMA_SGX_WC && count);
    if (fail_pages) { fail_pages--; *result = NULL; return -ENOMEM; }
    a = calloc(1, sizeof(*a)); assert(a);
    a->pages = calloc(count, sizeof(*a->pages)); assert(a->pages);
    data = calloc(count, PAGE_SIZE); assert(data);
    a->count = count;
    for (i = 0; i < count; i++) {
        a->pages[i] = calloc(1, sizeof(struct page)); assert(a->pages[i]);
        a->pages[i]->data = data + PAGE_SIZE * i;
    }
    pages_live += count; *result = a; return 0;
}
int gma3600_sgx_pages_free(struct gma3600_sgx_pages *a)
{
    unsigned i;
    if (fail_wb) { fail_wb--; return -EIO; }
    assert(a && pages_live >= a->count);
    pages_live -= a->count; free(a->pages[0]->data);
    for (i = 0; i < a->count; i++) free(a->pages[i]);
    free(a->pages); free(a); return 0;
}
static void *vmap(struct page **pages, unsigned count, unsigned flags, unsigned long prot)
{
    assert(count && flags == VM_MAP && prot == pgprot_writecombine(PAGE_KERNEL));
    if (fail_vmap) { fail_vmap--; return NULL; }
    mappings++; return pages[0]->data;
}
static void vunmap(void *p) { assert(p && mappings); mappings--; }

#include "buffer_provider.inc"

static struct vm_area_struct area(u32 token, unsigned size)
{
    return (struct vm_area_struct){.vm_start = 0x10000000, .vm_end = 0x10000000 + size,
        .vm_pgoff = token, .vm_flags = VM_SHARED | VM_MAYEXEC, .vm_page_prot = PAGE_KERNEL};
}
static void empty(void) { assert(!metadata && !pages_live && !mappings); }
static int visit_pages(void *context, struct page **pages, u32 count)
{
    struct gma_sgx_buffer *b = context;
    assert(pages == b->allocation->pages && count == b->allocation->count);
    assert(b->pool->lock.locked && b->refs == 2);
    return 17;
}

static void pinned_buffer_lifetime(void)
{
    struct gma_sgx_buffers *p = gma_sgx_buffers_create();
    struct sgx545_alloc_shared_out out;
    int owner, other;
    void *pin = (void *)1;
    assert(gma_sgx_buffer_pin(p, &owner, 1, &pin) == -ENOENT && !pin);
    assert(!gma_sgx_buffer_alloc(p, &owner, 1, SGX545_BUFFER_WC, 4096, &out));
    assert(gma_sgx_buffer_pin(p, &other, 1, &pin) == -ENOENT && !pin);
    assert(!gma_sgx_buffer_pin(p, &owner, 1, &pin) && pin);
    assert(!gma_sgx_buffer_free(p, &owner, 1) && pages_live == 1);
    gma_sgx_buffers_stop(p);
    assert(pages_live == 1); /* pin outlives both ownership and provider detach */
    gma_sgx_buffer_unpin(pin); empty();
}

static void ownership_transfer(void)
{
    struct gma_sgx_buffers *p = gma_sgx_buffers_create();
    struct sgx545_alloc_shared_out out;
    struct sgx545_map_info_out map;
    u32 tokens[2] = {1, 2}, bytes;
    int creator, stage;
    void *first_pin, *second_pin, *linear;
    struct vm_area_struct vma = area(1, 4096);
    struct vm_fault fault;
    struct sgx545_content_measure before, after;
    assert(!gma_sgx_buffer_alloc(p, &creator, 1, SGX545_BUFFER_WC, 4096, &out));
    assert(!gma_sgx_buffer_alloc(p, &creator, 2, SGX545_BUFFER_WC, 4096, &out));
    assert(!gma_sgx_buffer_mmap(p, &creator, &vma));
    assert(!gma_sgx_buffer_pin(p, &creator, 1, &first_pin));
    assert(!gma_sgx_buffer_pin(p, &creator, 2, &second_pin));
    assert(!gma_sgx_buffer_describe(p, &creator, 1, &linear, &bytes) && linear && bytes == 4096);
    ((unsigned char *)linear)[4095] = 17;
    assert(!gma_sgx_buffer_measure(p, &creator, 1, &before) && before.nonzero == 1);
    tokens[1] = 99;
    assert(gma_sgx_buffers_transfer(p, &creator, &stage, tokens, 2) == -ENOENT);
    assert(!gma_sgx_buffer_map_info(p, &creator, 1, &map)); /* no partial transfer */
    tokens[1] = 1;
    assert(gma_sgx_buffers_transfer(p, &creator, &stage, tokens, 2) == -EINVAL);
    tokens[1] = 2;
    assert(!gma_sgx_buffers_transfer(p, &creator, &stage, tokens, 2));
    assert(gma_sgx_buffer_measure(p, &creator, 1, &after) == -ENOENT);
    assert(!gma_sgx_buffer_measure(p, &stage, 1, &after) && after.hash == before.hash && after.nonzero == 1);
    assert(gma_sgx_buffer_map_info(p, &creator, 1, &map) == -ENOENT);
    gma_sgx_buffers_release_owner(p, &creator);
    assert(pages_live == 2 && !gma_sgx_buffer_map_info(p, &stage, 1, &map));
    assert(!gma_sgx_buffers_transfer(p, &stage, &creator, tokens, 2)); /* copyout rollback */
    assert(!gma_sgx_buffer_map_info(p, &creator, 1, &map));
    gma_sgx_buffer_unpin(first_pin); gma_sgx_buffer_unpin(second_pin);
    gma_sgx_buffers_stop(p);
    assert(pages_live == 1); /* VMA outlives transfer, rollback and provider detach. */
    assert(gma_sgx_buffer_measure(p, &creator, 1, &after) == -ENODEV && !after.hash);
    fault = (struct vm_fault){&vma, vma.vm_start, vma.vm_pgoff};
    assert(vma.vm_ops->fault(&fault) == VM_FAULT_NOPAGE);
    vma.vm_ops->close(&vma); empty();
}

int main(void)
{
    int owner, other;
    struct gma_sgx_buffers *p;
    struct sgx545_alloc_shared_out out;
    struct sgx545_map_info_out map;
    struct vm_area_struct vma, split;
    struct vm_fault fault;
    struct gma_sgx_buffer *b;
    volatile u32 *data;
    unsigned i;

    {
        const unsigned char fixture[] = {'h', 'e', 'l', 'l', 'o', 0, 3, 0};
        struct sgx545_content_measure measured = sgx545_measure_content(fixture, 5, 8);
        assert(measured.hash == 0x4f9f2cabU && measured.nonzero == 5 && measured.padding_nonzero == 1);
    }
    fail_alloc = 1;
    assert(!gma_sgx_buffers_create()); empty();
    p = gma_sgx_buffers_create(); assert(p);
    fail_alloc = 1;
    assert(gma_sgx_buffer_alloc(p, &owner, 1, SGX545_BUFFER_WC, 4096, &out) == -ENOMEM);
    fail_pages = 1;
    assert(gma_sgx_buffer_alloc(p, &owner, 1, SGX545_BUFFER_WC, 4096, &out) == -ENOMEM);
    assert(gma_sgx_buffer_alloc(p, &owner, 1, SGX545_BUFFER_WC, 0, &out) == -EINVAL);
    assert(gma_sgx_buffer_alloc(p, &owner, 1, 0, 4096, &out) == -EOPNOTSUPP);
    assert(gma_sgx_buffer_alloc(p, &owner, 1, SGX545_BUFFER_WC, SGX545_BUFFER_MAX_BYTES + 1, &out) == -EINVAL);
    fail_vmap = 1;
    assert(gma_sgx_buffer_alloc(p, &owner, 1, SGX545_BUFFER_WC, 4096, &out) == -ENOMEM);
    assert(!pages_live && !p->stats.buffers);
    assert(!gma_sgx_buffer_alloc(p, &owner, 42, SGX545_BUFFER_WC, 8193, &out));
    assert(out.mem.alloc_size == 8193 && out.mem.kernel_mem == 42 && !out.mem.kernel_linear);
    assert(gma_sgx_buffer_free(p, &other, 42) == -ENOENT);
    assert(!gma_sgx_buffer_map_info(p, &owner, 42, &map) && map.real_size == 12288 && map.offset == 42);
    vma = area(42, 12288); vma.vm_flags = 0;
    assert(gma_sgx_buffer_mmap(p, &owner, &vma) == -EACCES);
    vma.vm_flags = VM_SHARED | VM_EXEC;
    assert(gma_sgx_buffer_mmap(p, &owner, &vma) == -EACCES);
    vma = area(42, 8192);
    assert(gma_sgx_buffer_mmap(p, &owner, &vma) == -EINVAL);
    vma = area(42, 12288);
    assert(gma_sgx_buffer_mmap(p, &other, &vma) == -ENOENT);
    assert(!gma_sgx_buffer_mmap(p, &owner, &vma));
    assert((vma.vm_flags & (VM_DONTCOPY | VM_DONTEXPAND | VM_DONTDUMP)) ==
        (VM_DONTCOPY | VM_DONTEXPAND | VM_DONTDUMP) && !(vma.vm_flags & VM_MAYEXEC));
    b = vma.vm_private_data; assert(b->refs == 2 && pages_live == 3);
    assert(gma_sgx_buffer_visit(p, &other, 42, visit_pages, b) == -ENOENT);
    assert(gma_sgx_buffer_visit(p, &owner, 42, visit_pages, b) == 17);
    data = b->linear;
    {
        struct sgx545_content_measure zero, changed;
        assert(!gma_sgx_buffer_measure(p, &owner, 42, &zero) && !zero.nonzero && !zero.padding_nonzero);
        assert(gma_sgx_buffer_measure(p, &other, 42, &changed) == -ENOENT);
        ((unsigned char *)b->linear)[8192] = 7; /* final logical byte */
        ((unsigned char *)b->linear)[12287] = 9; /* final padding byte */
        assert(!gma_sgx_buffer_measure(p, &owner, 42, &changed));
        assert(changed.hash != zero.hash && changed.nonzero == 1 && changed.padding_nonzero == 1);
        assert(((unsigned char *)b->linear)[8192] == 7 && ((unsigned char *)b->linear)[12287] == 9);
        ((unsigned char *)b->linear)[8192] = ((unsigned char *)b->linear)[12287] = 0;
    }
    for (i = 0; i < 12288 / 4; i++) { assert(!data[i]); data[i] = 0x12345678U ^ i; }
    assert(gma_sgx_buffer_check(p, &owner, 42, 0, 0) == -EIO);
    assert(!gma_sgx_buffer_check(p, &owner, 42, 0x12345678U, 0xabcdef01U));
    for (i = 0; i < 12288 / 4; i++) assert(data[i] == (0xabcdef01U ^ i));
    split = vma; split.vm_start += 4096; split.vm_pgoff++;
    split.vm_ops->open(&split);
    fault = (struct vm_fault){&split, split.vm_start, split.vm_pgoff};
    assert(split.vm_ops->fault(&fault) == VM_FAULT_NOPAGE && inserted == b->allocation->pages[1]);
    fault.pgoff = 41; assert(split.vm_ops->fault(&fault) == VM_FAULT_SIGBUS);
    fault.pgoff = 45; assert(split.vm_ops->fault(&fault) == VM_FAULT_SIGBUS);
    assert(!gma_sgx_buffer_free(p, &owner, 42) && pages_live == 3 && b->refs == 2);
    assert(gma_sgx_buffer_map_info(p, &owner, 42, &map) == -ENOENT);
    split.vm_ops->close(&split); assert(pages_live == 3 && p->stats.maps == 1);
    gma_sgx_buffers_stop(p); /* base and owner gone; VMA keeps provider/pages alive */
    fault = (struct vm_fault){&vma, vma.vm_start, vma.vm_pgoff};
    assert(vma.vm_ops->fault(&fault) == VM_FAULT_NOPAGE && inserted == b->allocation->pages[0]);
    vma.vm_ops->close(&vma); empty();

    /* Detach with a still-owned buffer drops ownership but not the live VMA. */
    p = gma_sgx_buffers_create();
    assert(!gma_sgx_buffer_alloc(p, &owner, 1, SGX545_BUFFER_WC, 4096, &out));
    vma = area(1, 4096);
    assert(!gma_sgx_buffer_mmap(p, &owner, &vma));
    b = vma.vm_private_data;
    gma_sgx_buffers_stop(p);
    assert(!b->owner && b->refs == 1 && pages_live == 1);
    fault = (struct vm_fault){&vma, vma.vm_start, vma.vm_pgoff};
    assert(vma.vm_ops->fault(&fault) == VM_FAULT_NOPAGE);
    vma.vm_ops->close(&vma); empty();

    p = gma_sgx_buffers_create();
    for (i = 1; i <= BUFFER_OWNER_MAX; i++)
        assert(!gma_sgx_buffer_alloc(p, &owner, i, SGX545_BUFFER_WC, 4096, &out));
    assert(gma_sgx_buffer_alloc(p, &owner, 99, SGX545_BUFFER_WC, 4096, &out) == -ENOSPC);
    gma_sgx_buffers_release_owner(p, &owner);
    assert(!pages_live && !p->stats.buffers);
    for (i = 1; i <= BUFFER_MAX_COUNT; i++)
        assert(!gma_sgx_buffer_alloc(p, (void *)(uintptr_t)i, i, SGX545_BUFFER_WC, 4096, &out));
    assert(gma_sgx_buffer_alloc(p, &owner, 99, SGX545_BUFFER_WC, 4096, &out) == -ENOSPC);
    gma_sgx_buffers_stop(p); empty();

    /* A freed-but-mapped buffer keeps its physical-page quota charged. */
    p = gma_sgx_buffers_create();
    for (i = 1; i <= 16; i++)
        assert(!gma_sgx_buffer_alloc(p, &owner, i, SGX545_BUFFER_WC, SGX545_BUFFER_MAX_BYTES, &out));
    assert(p->stats.pages == BUFFER_MAX_PAGES);
    vma = area(1, SGX545_BUFFER_MAX_BYTES);
    assert(!gma_sgx_buffer_mmap(p, &owner, &vma));
    assert(!gma_sgx_buffer_free(p, &owner, 1));
    assert(gma_sgx_buffer_alloc(p, &other, 99, SGX545_BUFFER_WC, 4096, &out) == -ENOSPC);
    vma.vm_ops->close(&vma);
    assert(!gma_sgx_buffer_alloc(p, &other, 99, SGX545_BUFFER_WC, 4096, &out));
    gma_sgx_buffers_stop(p); empty();

    p = gma_sgx_buffers_create();
    assert(!gma_sgx_buffer_alloc(p, &owner, 1, SGX545_BUFFER_WC, 4096, &out));
    fail_wb = 1;
    assert(gma_sgx_buffer_free(p, &owner, 1) == -EIO);
    assert(p->stats.errors == 1 && p->stats.pages == 1 && pages_live == 1 && !mappings);
    b = container_of(p->buffers.next, struct gma_sgx_buffer, link);
    assert(!b->refs && !b->owner);
    /* Test-only recovery of deliberately retained cache-failure metadata. */
    b->refs = 1; buffer_put_locked(b);
    gma_sgx_buffers_stop(p); empty();
    pinned_buffer_lifetime();
    ownership_transfer();
    puts("SGX buffer tests passed: independent mapping pins, ownership, zeroing, WC aliases, VMA split/detach lifetime, quotas and cache-failure retention");
    return 0;
}
