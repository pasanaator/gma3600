/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA3600_GTT_TEST_STUBS_H
#define GMA3600_GTT_TEST_STUBS_H
#define __GMA3600_DRV_H__
#define __iomem
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <stddef.h>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint64_t dma_addr_t;
typedef uint64_t resource_size_t;
#define PAGE_SIZE 4096UL
#define GFP_KERNEL 0
#define BITS_TO_LONGS(n) (((n) + 8 * sizeof(unsigned long) - 1) / (8 * sizeof(unsigned long)))
#define lower_32_bits(n) ((u32)(n))
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_warn(...) ((void)0)
struct mutex { int unused; };
static inline void mutex_init(struct mutex *m) { (void)m; }
static inline void mutex_lock(struct mutex *m) { (void)m; }
static inline void mutex_unlock(struct mutex *m) { (void)m; }
static inline void *kzalloc(size_t n, int flags) { (void)flags; return calloc(1, n); }
#define kfree free
#define wmb() ((void)0)
static inline u32 ioread32(void *p) { return *(u32 *)p; }
static inline void iowrite32(u32 v, void *p) { *(u32 *)p = v; }
struct pci_dev { u16 gmch; };
int pci_read_config_word(struct pci_dev *p, unsigned int reg, u16 *value);
int pci_write_config_word(struct pci_dev *p, unsigned int reg, u16 value);
static inline int pcibios_err_to_errno(int ret) { return ret ? -EIO : 0; }
static inline resource_size_t pci_resource_len(struct pci_dev *p, int bar)
{ (void)p; (void)bar; return 0; }
void *ioremap(resource_size_t phys, size_t size);
void iounmap(void *p);
struct gma3600_gtt {
    resource_size_t gtt_phys;
    void *gtt_map;
    u32 saved_pgetbl_ctl;
    u16 saved_gmch_ctrl;
    u32 gtt_total_entries, gtt_stolen_entries, stolen_base;
    unsigned long *gtt_free_bitmap;
    struct mutex lock;
};
struct gma3600_device {
    struct pci_dev *pdev;
    resource_size_t aperture_size;
    struct gma3600_gtt gtt;
    u32 pgetbl;
};
struct gma3600_gem_object {
    bool in_gtt, stolen;
    u32 gtt_num_pages, num_pages, gtt_offset;
    dma_addr_t *dma_addrs;
};
#define GMA3600_GTT_PAGE_SIZE 4096
u32 gma3600_read(struct gma3600_device *dev, u32 reg);
void gma3600_write_post(struct gma3600_device *dev, u32 reg, u32 value);
static inline void bitmap_set(unsigned long *map, unsigned long start, unsigned long n)
{ while (n--) { map[start / (8*sizeof(*map))] |= 1UL << (start % (8*sizeof(*map))); start++; } }
static inline void bitmap_clear(unsigned long *map, unsigned long start, unsigned long n)
{ while (n--) { map[start / (8*sizeof(*map))] &= ~(1UL << (start % (8*sizeof(*map)))); start++; } }
static inline unsigned long bitmap_find_next_zero_area(unsigned long *map,
    unsigned long size, unsigned long start, unsigned long n, unsigned long mask)
{
    unsigned long i, j;
    (void)mask;
    for (i = start; i + n <= size; i++) {
        for (j = 0; j < n; j++)
            if (map[(i+j)/(8*sizeof(*map))] & (1UL << ((i+j)%(8*sizeof(*map))))) break;
        if (j == n) return i;
    }
    return size;
}
int gma3600_gtt_init(struct gma3600_device *dev);
void gma3600_gtt_fini(struct gma3600_device *dev);
int gma3600_gtt_insert(struct gma3600_device *dev, struct gma3600_gem_object *obj);
void gma3600_gtt_remove(struct gma3600_device *dev, struct gma3600_gem_object *obj);
#endif
