// SPDX-License-Identifier: GPL-2.0-only
/* Actual native SGX structures and InitDevInfo/DeinitDevInfo bodies. The outer
 * device container and OS allocation/barrier seams are mocked, not the binder. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pvr_bridge.h"
#include "sgx_bridge.h"
#include "sgx_options.h"
typedef struct _PVRSRV_DEVICE_NODE_ {
    PVRSRV_DEVICE_IDENTIFIER sDevId;
    void *pvDevice;
} PVRSRV_DEVICE_NODE;
#include "sgxinfokm.h"
#define SGX545_FREESTANDING
#include "gma3600_sgx_init.h"
#include "os_seam.h"
typedef struct _PVRSRV_PER_PROCESS_DATA_ PVRSRV_PER_PROCESS_DATA;

static unsigned allocated, calls, fail_at;
static PVRSRV_ERROR allocate_(unsigned heap, size_t size, void **out)
{
    assert(heap == PVRSRV_OS_NON_PAGEABLE_HEAP);
    if (++calls == fail_at) return PVRSRV_ERROR_OUT_OF_MEMORY;
    *out = calloc(1, size); assert(*out); allocated++;
    return PVRSRV_OK;
}
static void free_(void *memory) { assert(memory && allocated); allocated--; free(memory); }
#undef OSAllocMem
#undef OSFreeMem
#undef OSMemCopy
#undef OSMemSet
#undef OSMemoryBarrier
#undef PVR_DPF
#define OSAllocMem(heap, size, out, handle, name) allocate_((heap), (size), (void **)(out))
#define OSFreeMem(heap, size, ptr, handle) ((void)sizeof(char[(heap) == PVRSRV_OS_NON_PAGEABLE_HEAP ? 1 : -1]), free_(ptr))
#define OSMemCopy memcpy
#define OSMemSet memset
#define OSMemoryBarrier() __sync_synchronize()
#define PVR_DPF(args) ((void)0)
#define OSClockus() 123456U
static PVRSRV_ERROR reset_result;
PVRSRV_ERROR SGXReset(PVRSRV_SGXDEV_INFO *d, IMG_BOOL recovery, IMG_UINT32 flags)
{ assert(d && recovery && !flags && d->ui32CoreClockSpeed); return reset_result; }
#include "native_functions.inc"

static struct gma_sgx_native_init snapshot;
static struct gma_sgx_init_memory memory[15];
static PVRSRV_SGXDEV_INFO dev, before;
static IMG_UINT32 reset_pd[1024], reset_pt[1024];
static PVRSRV_DEVICE_NODE node;
static void setup(void)
{
    struct sgx545_init_requirements caps = {0};
    struct sgx545_init2_in32 wire = {.device = 2};
    unsigned i;
    memset(&dev, 0, sizeof(dev));
    dev.pvRegsBaseKM = (void *)(uintptr_t)0x100000000ULL;
    dev.ui32CacheControl = 0x80;
    dev.sKernelPDDevPAddr.uiAddr = 0x12345000;
    dev.sBIFResetPDDevPAddr.uiAddr = 0x22345000;
    dev.sBIFResetPTDevPAddr.uiAddr = 0x22346000;
    dev.sBIFResetPageDevPAddr.uiAddr = 0x22347000;
    dev.pui32BIFResetPD=reset_pd; dev.pui32BIFResetPT=reset_pt;
    before = dev;
    node.pvDevice = &dev; node.sDevId.eDeviceType = PVRSRV_DEVICE_TYPE_SGX;
    gma_sgx_init_requirements(&caps);
    wire.info.ccb = 1; wire.info.ccb_control = 2; wire.info.event_kicker = 3;
    wire.info.host_control = 4; wire.info.ta3d_control = 5; wire.info.misc = 6;
    wire.info.hwperf = 7; wire.info.ta_signature = 8; wire.info.signature_3d = 9;
    wire.info.build_options = caps.build_options;
    memcpy(wire.info.struct_sizes, caps.struct_sizes, sizeof(caps.struct_sizes));
    wire.info.scripts.part1[0].op = wire.info.scripts.part2[0].op = wire.info.scripts.deinit[0].op = 2;
    for (i = 0; i < 15; i++) {
        unsigned bytes = i < 9 ? caps.minimum_bytes[i] : 4096;
        bytes = (bytes + 4095) & ~4095U;
        memory[i] = (struct gma_sgx_init_memory){i + 1, 0xf4000000U + i * 0x100000U, bytes,
            SGX545_HEAP_FLAGS | SGX545_INTEL_INIT_FLAGS, calloc(1, bytes)};
        assert(memory[i].linear);
        if (i >= 9) wire.info.mem_handles[i - 9] = i + 1;
    }
    assert(!gma_sgx_init_translate(&wire, memory, 15, &snapshot));
    calls = fail_at = 0;
}
static void teardown(void)
{
    unsigned i;
    assert(!allocated && !memcmp(&dev, &before, sizeof(dev)));
    for (i = 0; i < 15; i++) free(memory[i].linear);
}
int main(void)
{
    unsigned attempt, i;
    for (attempt = 0; attempt < 3; attempt++) {
        struct sgx545_native_cycle result = {0};
        setup(); fail_at = attempt;
        assert(gma3600_sgx_native_cycle(&node, &snapshot, &result) == (attempt ? -ENOMEM : 0));
        if (!attempt) assert(result.bound == 1 && result.restored == 1 && result.flags == SGX545_NATIVE_CCB_ROUNDTRIP && result.ccb_bytes == 8192 && result.slots == 256);
        for (i = 0; i < 8192; i++) assert(!((unsigned char *)memory[0].linear)[i]);
        teardown();
    }
    setup();
    {
        struct sgx545_native_cycle result = {0};
        PVRSRV_SGX_CCB_CTL *control = memory[1].linear;
        control->ui32ReadOffset = control->ui32WriteOffset = 255;
        assert(!gma3600_sgx_native_cycle(&node, &snapshot, &result) && result.read_offset == 255 && result.write_offset == 255);
        for (i = 0; i < 8192; i++) assert(!((unsigned char *)memory[0].linear)[i]);
        control->ui32WriteOffset = 256;
        assert(gma3600_sgx_native_cycle(&node, &snapshot, &result) == -EINVAL);
        control->ui32WriteOffset = 254;
        assert(gma3600_sgx_native_cycle(&node, &snapshot, &result) == -EAGAIN);
        control->ui32WriteOffset = 255;
        snapshot.info.hKernelCCBMemInfo = (void *)1;
        assert(gma3600_sgx_native_cycle(&node, &snapshot, &result) == -EINVAL);
        snapshot.info.hKernelCCBMemInfo = &snapshot.memory[0];
        dev.psKernelCCBInfo = (void *)1;
        assert(gma3600_sgx_native_cycle(&node, &snapshot, &result) == -EBUSY);
        dev.psKernelCCBInfo = NULL;
        snapshot.memory[0].ui32Flags &= ~SGX545_DEVICE_WRITE;
        assert(gma3600_sgx_native_cycle(&node, &snapshot, &result) == -EOPNOTSUPP);
    }
    teardown();
    setup();
    {
        void *runtime = NULL, *second = NULL;
        struct sgx545_native_cycle result = {.services = 12, .stage = 13, .device = 14};
        PVRSRV_SGX_CCB_CTL *control = memory[1].linear;
        snapshot.info.ui32CacheControl = 0x40;
        assert(!gma3600_sgx_native_begin(&node, &snapshot, &runtime, &result));
        assert(runtime && allocated == 2 && result.bound && !result.restored && dev.psKernelCCBInfo);
        assert((dev.ui32CacheControl & 0xc0) == 0xc0);
        assert(gma3600_sgx_native_begin(&node, &snapshot, &second, &result) == -EBUSY && !second);
        dev.ui32CacheControl |= 0x1000; /* Interleaved existing-heap PTE cleanup. */
        assert(!gma3600_sgx_native_query(runtime, &result) && result.services == 12 && result.stage == 13 && result.device == 14);
        control->ui32WriteOffset = 1;
        assert(gma3600_sgx_native_query(runtime, &result) == -EUCLEAN);
        control->ui32WriteOffset = 0;
        assert(!gma3600_sgx_native_query(runtime, &result));
        assert(!gma3600_sgx_native_end(&runtime) && !runtime && !allocated);
        before.ui32CacheControl |= 0x1040;
        assert(!memcmp(&dev, &before, sizeof(dev)));
        assert(gma3600_sgx_native_end(&runtime) == -ENOENT);
        assert(!gma3600_sgx_native_begin(&node, &snapshot, &runtime, &result));
        dev.psKernelCCBInfo = (void *)1; /* Never dereference/free an alien CCB. */
        assert(gma3600_sgx_native_query(runtime, &result) == -EUCLEAN);
        assert(gma3600_sgx_native_end(&runtime) == -EUCLEAN && !runtime && !allocated);
        assert(!memcmp(&dev, &before, sizeof(dev)));
    }
    teardown();
    setup();
    {
        void *runtime=NULL;
        struct sgx545_native_cycle result={0};
        struct gma_sgx_live_roots roots;
        assert(!gma3600_sgx_native_begin(&node,&snapshot,&runtime,&result));
#if SGX_CORE_REV == 10131
        assert(!gma3600_sgx_native_live_prepare(runtime,dev.pvRegsBaseKM,&roots));
        assert(roots.kernel_pd==0x12345000 && roots.reset_pd==0x22345000 && roots.edm_dir==0xc70 && roots.bank==15);
        assert(!gma3600_sgx_native_query(runtime,&result));
        assert(!gma3600_sgx_native_live_arm(runtime));
        assert(gma3600_sgx_native_query(runtime,&result)==-EBUSY);
        assert(gma3600_sgx_native_end(&runtime)==-EBUSY && runtime);
        assert(dev.psSGXHostCtl->ui32PowerStatus==PVRSRV_USSE_EDM_POWMAN_NO_WORK);
        assert(dev.psSGXHostCtl->ui32uKernelTimerClock==400000 && !dev.psSGXHostCtl->ui32ActivePowManSampleRate);
        assert(!gma3600_sgx_native_live_publish(runtime) && dev.psSGXHostCtl->ui32HostClock==123456);
        dev.psSGXHostCtl->ui32InitStatus=1;
        { __u32 status=0, error=0;
          assert(!gma3600_sgx_native_live_status(runtime,&status) && status==1);
          reset_result=PVRSRV_ERROR_TIMEOUT;
          assert(gma3600_sgx_native_live_reset(runtime,&error)==-ETIMEDOUT && error==PVRSRV_ERROR_TIMEOUT);
          reset_result=0;assert(!gma3600_sgx_native_live_reset(runtime,&error) && !error);
        }
        /* Test-only power-cycle cleanup; no production unarm API exists. */
        ((struct gma_sgx_native_runtime_state *)runtime)->hardware_owned=0;
#else
        assert(gma3600_sgx_native_live_prepare(runtime,dev.pvRegsBaseKM,&roots)==-EOPNOTSUPP);
#endif
        assert(!gma3600_sgx_native_end(&runtime));
    }
    teardown();
    puts("Native SGX CPU/live helpers passed: binding lifetime, live timing/host control, native reset result, retained hardware binding and unsupported-profile rejection");
    return 0;
}
