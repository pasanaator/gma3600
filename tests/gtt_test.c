// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include "gtt_stubs.h"
#include "gma3600_reg.h"

static bool fail_read, fail_write, ignore_write;
static int reads, fail_read_at;
static int mappings;

int pci_read_config_word(struct pci_dev *p, unsigned int reg, u16 *value)
{
    assert(reg == 0x52);
    reads++;
    if (fail_read || reads == fail_read_at) return 1;
    *value = p->gmch;
    return 0;
}
int pci_write_config_word(struct pci_dev *p, unsigned int reg, u16 value)
{
    assert(reg == 0x52);
    if (fail_write) return 1;
    if (!ignore_write) p->gmch = value;
    return 0;
}
void *ioremap(resource_size_t phys, size_t size)
{
    assert(phys == 0x12345000);
    mappings++;
    return calloc(1, size);
}
void iounmap(void *p) { assert(p); mappings--; free(p); }
u32 gma3600_read(struct gma3600_device *dev, u32 reg)
{ assert(reg == PGETBL_CTL); return dev->pgetbl; }
void gma3600_write_post(struct gma3600_device *dev, u32 reg, u32 value)
{ assert(reg == PGETBL_CTL); dev->pgetbl = value; }

int main(void)
{
    struct pci_dev pci = { .gmch = 0x1200 };
    struct gma3600_device dev = { .pdev = &pci, .aperture_size = 128UL << 20,
        .pgetbl = 0x12345000 };
    dma_addr_t pages[] = { 0x23456000, 0x34567000 };
    struct gma3600_gem_object obj = { .num_pages = 2, .gtt_num_pages = 2,
        .dma_addrs = pages };

    assert(!gma3600_gtt_init(&dev));
    assert(pci.gmch == 0x1204); /* Enable bit, preserving unrelated PCI controls. */
    assert(dev.pgetbl == 0x12345001);
    assert(!gma3600_gtt_insert(&dev, &obj));
    assert(obj.in_gtt && obj.gtt_offset == 0);
    assert(ioread32(dev.gtt.gtt_map) == 0x23456009);
    assert(ioread32(dev.gtt.gtt_map + 4) == 0x34567009);
    gma3600_gtt_remove(&dev, &obj);
    assert(!obj.in_gtt && ioread32(dev.gtt.gtt_map) == 0);
    gma3600_gtt_fini(&dev);
    assert(pci.gmch == 0x1200 && dev.pgetbl == 0x12345000);
    assert(!mappings);

    fail_read = true;
    assert(gma3600_gtt_init(&dev) == -EIO);
    assert(!mappings && pci.gmch == 0x1200);
    fail_read = false;
    fail_write = true;
    assert(gma3600_gtt_init(&dev) == -EIO);
    assert(!mappings && pci.gmch == 0x1200);
    assert(!dev.gtt.gtt_map && !dev.gtt.gtt_free_bitmap);
    fail_write = false;
    fail_read_at = reads + 2; /* Fail the enable readback, after a successful write. */
    assert(gma3600_gtt_init(&dev) == -EIO);
    assert(!mappings && pci.gmch == 0x1200);
    fail_read_at = 0;
    ignore_write = true; /* PCI write succeeds but the enable bit does not stick. */
    assert(gma3600_gtt_init(&dev) == -EIO);
    assert(!mappings && pci.gmch == 0x1200);
    puts("GTT regression tests passed: GMCH enable/restore, PTE mappings, PCI failure cleanup");
    return 0;
}
