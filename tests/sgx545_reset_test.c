// SPDX-License-Identifier: GPL-2.0-only
/* Compiles the patched DDK function with a mock OS, never a kernel/device. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t IMG_UINT32;
typedef unsigned char IMG_BYTE;
typedef void IMG_VOID;
typedef void *IMG_HANDLE;
typedef void *IMG_PVOID;
typedef uintptr_t IMG_UINTPTR_T;
typedef int PVRSRV_ERROR;
typedef unsigned pgprot_t;
static pgprot_t pgprot_writecombine(pgprot_t value) { return (value & ~3U) | 1; }
static pgprot_t pgprot_noncached(pgprot_t value) { return (value & ~3U) | 2; }
#include "services4/srvkm/env/linux/mutils.h"
typedef struct { IMG_UINT32 uiAddr; } IMG_CPU_PHYADDR;
typedef struct { IMG_UINT32 uiAddr; } IMG_DEV_PHYADDR;
typedef struct { IMG_UINT32 uiAddr; } IMG_SYS_PHYADDR;
typedef void RA_ARENA;
typedef struct { RA_ARENA *apsLocalDevMemArena[1]; } SYS_DATA;
typedef struct {
    IMG_HANDLE hBIFResetPDOSMemHandle;
    IMG_DEV_PHYADDR sBIFResetPDDevPAddr, sBIFResetPTDevPAddr, sBIFResetPageDevPAddr;
    IMG_UINT32 *pui32BIFResetPD, *pui32BIFResetPT;
} PVRSRV_SGXDEV_INFO;
#define IMG_NULL NULL
#define IMG_TRUE 1
#define PVRSRV_OK 0
#define PVRSRV_ERROR_OUT_OF_MEMORY 1
#define PVRSRV_ERROR_BAD_MAPPING 2
#define PVRSRV_HAP_WRITECOMBINE 1
#define PVRSRV_HAP_KERNEL_ONLY 2
#define SGX_MMU_PAGE_SIZE 4096
#define PVRSRV_DEVICE_TYPE_SGX 1
#define PVR_DPF(args) ((void)0)
#define PVR_ASSERT(value) assert(value)
#define PVR_UNREFERENCED_PARAMETER(value) ((void)(value))
#define OSMemSet memset

static SYS_DATA system_data;
static _Alignas(4096) IMG_BYTE memory[3 * SGX_MMU_PAGE_SIZE];
static const IMG_UINT32 physical[] = { 0x100000, 0x900000, 0x2a0000 };
static int fail_alloc, blocked_accesses;
static void SysAcquireData(SYS_DATA **data) { *data = &system_data; }
static PVRSRV_ERROR OSAllocPages(unsigned flags, unsigned bytes, unsigned align,
    void *priv, unsigned priv_size, void *bm, void **cpu, void **handle)
{
    assert(flags == 3 && bytes == sizeof(memory) && align == SGX_MMU_PAGE_SIZE);
    assert(!priv && !priv_size && !bm);
    if (fail_alloc) return PVRSRV_ERROR_OUT_OF_MEMORY;
    *cpu = memory;
    *handle = memory;
    return PVRSRV_OK;
}
static IMG_CPU_PHYADDR OSMapLinToCPUPhys(void *handle, void *cpu)
{ assert(handle == memory && cpu == memory); return (IMG_CPU_PHYADDR){physical[0]}; }
static IMG_CPU_PHYADDR OSMemHandleToCpuPAddr(void *handle, unsigned offset)
{
    assert(handle == memory && offset < sizeof(memory) && !(offset % 4096));
    return (IMG_CPU_PHYADDR){physical[offset / 4096]};
}
static IMG_DEV_PHYADDR SysCpuPAddrToDevPAddr(unsigned type, IMG_CPU_PHYADDR p)
{ assert(type == PVRSRV_DEVICE_TYPE_SGX); return (IMG_DEV_PHYADDR){p.uiAddr}; }
static IMG_CPU_PHYADDR SysSysPAddrToCpuPAddr(IMG_SYS_PHYADDR p)
{ return (IMG_CPU_PHYADDR){p.uiAddr}; }
static int RA_Alloc(void *arena, ...)
{ (void)arena; assert(0); return 0; }
static void *OSMapPhysToLin(IMG_CPU_PHYADDR p, unsigned bytes, unsigned flags, void **h)
{ (void)p; (void)bytes; (void)flags; (void)h; assert(0); return NULL; }
static void gma3600_sgx_blocked_write(IMG_UINT32 offset, IMG_UINT32 value)
{ (void)offset; (void)value; blocked_accesses++; }
static int gma3600_sgx_live_read(void *base, unsigned offset, unsigned *value)
{ (void)base; (void)offset; (void)value; return 0; }
static int gma3600_sgx_live_write(void *base, unsigned offset, unsigned value)
{ (void)base; (void)offset; (void)value; return 0; }

#include "sgx545_source_functions.inc"

int main(void)
{
    PVRSRV_SGXDEV_INFO info = {0};
    memset(memory, 0xab, sizeof(memory));
    assert(MMU_BIFResetPDAlloc(&info) == PVRSRV_OK);
    assert(info.sBIFResetPDDevPAddr.uiAddr == physical[0]);
    assert(info.sBIFResetPTDevPAddr.uiAddr == physical[1]);
    assert(info.sBIFResetPageDevPAddr.uiAddr == physical[2]);
    for (unsigned i = 0; i < sizeof(memory); i++)
        assert(memory[i] == (i < 8192 ? 0 : 0xdb));
    memset(&info, 0, sizeof(info));
    fail_alloc = 1;
    assert(MMU_BIFResetPDAlloc(&info) == PVRSRV_ERROR_OUT_OF_MEMORY);
    assert(!info.hBIFResetPDOSMemHandle);
    OSWriteHWReg(NULL, 0xdead, 1); /* A NULL base must never be dereferenced. */
    assert(OSReadHWReg(NULL, 0xbeef) == 0);
    assert(blocked_accesses == 2);
    assert(PGPROT_WC(0x100) == 0x101);
    assert(PGPROT_UC(0x100) == 0x102);
    puts("Patched DDK tests passed: noncontiguous reset pages, allocation failure, blocked MMIO, amd64 PAT protections");
    return 0;
}
