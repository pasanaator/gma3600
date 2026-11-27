// SPDX-License-Identifier: GPL-2.0-only
/* Actual SGXInitialise/SGXReset bodies, real native structures/register masks.
 * Only MMIO, time, polling, debug output and PDump boundaries are simulated. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pvr_bridge.h"
#include "sgx_bridge.h"
typedef struct _PVRSRV_DEVICE_NODE_ PVRSRV_DEVICE_NODE;
#include "sgxinfokm.h"
#include "sgxconfig.h"
#include "sgxmmu.h"
#include "sysinfo.h"
#include "gma3600_sgx_boot.h"
#include "gma3600_sgx_live_policy.h"
#include "fixtures/sgx545_boot_capture.h"

_Static_assert(EUR_CR_EVENT_KICK2 == GMA_SGX_BOOT_KICK2, "boot kick register");
_Static_assert(PVRSRV_USSE_EDM_INIT_COMPLETE == GMA_SGX_BOOT_INIT_COMPLETE, "boot completion bit");
_Static_assert(EUR_CR_EVENT_HOST_ENABLE == 0x130 && EUR_CR_EVENT_HOST_ENABLE2 == 0x110, "boot IRQ masks");

#ifdef TEST_SINGLE_CONTEXT
#undef SGX_FEATURE_MULTIPLE_MEM_CONTEXTS
#endif
static PVRSRV_SGXDEV_INFO dev;
static PVRSRV_KERNEL_MEM_INFO host_mem;
static SGXMKIF_HOST_CTL host;
static PVRSRV_SGX_CCB_CTL ccb;
static IMG_UINT32 reset_pd[1024], reset_pt[1024], kicker;
static IMG_UINT32 registers_[0x4000 / 4];
static struct { unsigned offset, value; } writes[4096];
static unsigned write_count, fault_reads, faults, fault_address, waits_us, polls, kicks;
static unsigned fail_poll, init_complete;
static unsigned captured_replay;
static unsigned reset_scope;

static void write_reg(void *base, unsigned offset, unsigned value)
{
    assert(base == registers_ && !(offset & 3) && offset < sizeof(registers_));
    assert(write_count < sizeof(writes) / sizeof(writes[0]));
    writes[write_count].offset = offset; writes[write_count++].value = value;
    if(captured_replay) {
        struct gma_sgx_live_roots roots={dev.sKernelPDDevPAddr.uiAddr >> SGX_MMU_PDE_ADDR_ALIGNSHIFT,
            dev.sBIFResetPDDevPAddr.uiAddr,EUR_CR_BIF_DIR_LIST_BASE1+4*(SGX_BIF_DIR_LIST_INDEX_EDM-1),SGX_BIF_DIR_LIST_INDEX_EDM,0};
        if(offset==EUR_CR_SOFT_RESET && value)reset_scope=1;
        if(reset_scope)assert(gma_sgx_reset_write_allowed(&roots,offset,value));
        if(offset==EUR_CR_SOFT_RESET && !value)reset_scope=0;
    }
    registers_[offset / 4] = value;
    if (captured_replay && (offset == EUR_CR_EVENT_HOST_ENABLE || offset == EUR_CR_EVENT_HOST_ENABLE2))
        assert(value == 0);
    if (offset == EUR_CR_EVENT_KICK2) {
        assert(value == EUR_CR_EVENT_KICK2_NOW_MASK);
        /* The PD must be installed and all reset bits released first. */
        assert(registers_[EUR_CR_SOFT_RESET / 4] == 0);
        assert(registers_[(SGX_BIF_DIR_LIST_INDEX_EDM == 0 ? EUR_CR_BIF_DIR_LIST_BASE0 :
                           EUR_CR_BIF_DIR_LIST_BASE1 + 4 * (SGX_BIF_DIR_LIST_INDEX_EDM - 1)) / 4] ==
               (dev.sKernelPDDevPAddr.uiAddr >> SGX_MMU_PDE_ADDR_ALIGNSHIFT));
        assert(host.ui32InitStatus == 0 && host.ui32HostClock == 123456);
        kicks++;
    }
}
static unsigned read_reg(void *base, unsigned offset)
{
    if(captured_replay && reset_scope)assert(gma_sgx_reset_read_allowed(offset));
    assert(base == registers_ && !(offset & 3) && offset < sizeof(registers_));
    if (offset == EUR_CR_BIF_INT_STAT) {
        fault_reads++;
        return fault_reads <= faults ? EUR_CR_BIF_INT_STAT_FAULT_REQ_MASK : 0;
    }
    if (offset == EUR_CR_BIF_FAULT) return fault_address;
    return registers_[offset / 4];
}
static PVRSRV_ERROR poll_value(volatile IMG_UINT32 *address, unsigned value, unsigned mask,
                              unsigned timeout, unsigned delay, IMG_BOOL preempt)
{
    assert(timeout == MAX_HW_TIME_US && delay == MAX_HW_TIME_US / WAIT_TRY_COUNT && !preempt);
    if (++polls == fail_poll) return PVRSRV_ERROR_TIMEOUT;
    if (address == &host.ui32InitStatus) {
        assert(kicks == 1);
        if (!init_complete) return PVRSRV_ERROR_TIMEOUT;
        host.ui32InitStatus = PVRSRV_USSE_EDM_INIT_COMPLETE;
    } else {
        assert(address == registers_ + EUR_CR_BIF_MEM_REQ_STAT / 4);
    }
    assert((*address & mask) == value);
    return PVRSRV_OK;
}
#undef PVR_DPF
#undef PVR_DBG_BREAK
#define PVR_DPF(args) ((void)0)
#define PVR_DBG_BREAK ((void)0)
#define PVR_ASSERT assert
#define OSWriteHWReg write_reg
#define OSReadHWReg read_reg
#define PollForValueKM poll_value
#define OSWaitus(delay) (assert((delay) > 0 && (delay) < 1000), waits_us += (delay))
#define OSClockus() 123456U
#define OSMemoryBarrier() __sync_synchronize()
#define OSWriteMemoryBarrier() __sync_synchronize()
#define SGXDumpDebugInfo(device, full) ((void)(device), (void)(full))
#define PDUMP_FLAGS_CONTINUOUS 0
#define PDUMPCOMMENTWITHFLAGS(...) ((void)0)
#define PDUMPREGWITHFLAGS(...) ((void)0)
#define PDUMPREG(...) ((void)0)
#define PDUMPCOMMENT(...) ((void)0)
#define PDUMPIDLWITHFLAGS(...) ((void)0)
#define PDUMPPDREGWITHFLAGS(...) ((void)0)
#define PDUMPREGPOLWITHFLAGS(...) ((void)0)
#include "firmware_functions.inc"

static void setup(void)
{
    memset(&dev, 0, sizeof(dev)); memset(&host, 0, sizeof(host));
    memset(registers_, 0, sizeof(registers_)); memset(reset_pd, 0, sizeof(reset_pd));
    memset(reset_pt, 0, sizeof(reset_pt));
    write_count = fault_reads = faults = waits_us = polls = kicks = fail_poll = 0;
    init_complete = 1; fault_address = 0xffffffffU; kicker = 0;
    dev.pvRegsBaseKM = registers_; dev.sKernelPDDevPAddr.uiAddr = 0x12345000;
    dev.ui32CoreClockSpeed = 400000000;
    dev.sBIFResetPDDevPAddr.uiAddr = 0x22345000;
    dev.sBIFResetPTDevPAddr.uiAddr = 0x22346000;
    dev.sBIFResetPageDevPAddr.uiAddr = 0x22347000;
    dev.pui32BIFResetPD = reset_pd; dev.pui32BIFResetPT = reset_pt;
    host_mem.pvLinAddrKM = &host; dev.psKernelSGXHostCtlMemInfo = &host_mem;
    dev.psKernelCCBCtl = &ccb; dev.pui32KernelCCBEventKicker = &kicker;
    dev.sScripts.asInitCommandsPart1[0].sWriteHWReg.eOp = SGX_INIT_OP_WRITE_HW_REG;
    dev.sScripts.asInitCommandsPart1[0].sWriteHWReg.ui32Offset = EUR_CR_EVENT_HOST_ENABLE;
    dev.sScripts.asInitCommandsPart1[1].eOp = SGX_INIT_OP_HALT;
    dev.sScripts.asInitCommandsPart2[0].sWriteHWReg.eOp = SGX_INIT_OP_WRITE_HW_REG;
    dev.sScripts.asInitCommandsPart2[0].sWriteHWReg.ui32Offset = EUR_CR_EVENT_HOST_ENABLE2;
    dev.sScripts.asInitCommandsPart2[1].eOp = SGX_INIT_OP_HALT;
}
static void assert_contained(void)
{
    unsigned i;
    assert(registers_[EUR_CR_SOFT_RESET / 4] & EUR_CR_SOFT_RESET_BIF_RESET_MASK);
    assert(!kicks); /* No second script or first kick on reset failure. */
    for (i = 0; i < write_count; i++) assert(writes[i].offset != EUR_CR_EVENT_HOST_ENABLE2);
}
int main(void)
{
    unsigned i, part1 = ~0U, part2 = ~0U, first_reset = ~0U, last_reset = 0, kick = 0;
    /* Successful first start, one fault drained at the largest VA, exact
     * native reset table cleanup, installed original PD and first kick. */
    setup(); faults = 1;
    assert(SGXInitialise(&dev, IMG_TRUE) == PVRSRV_OK);
    assert(kicks == 1 && host.ui32InitStatus == PVRSRV_USSE_EDM_INIT_COMPLETE);
    assert(!kicker); /* SGX545 uses EVENT_KICK2, not the older RAM kicker. */
    for (i = 0; i < 1024; i++) assert(!reset_pd[i] && !reset_pt[i]);
    assert(fault_reads == 2 && waits_us > 0);
    for (i = 0; i < write_count; i++) {
        if (writes[i].offset == EUR_CR_EVENT_HOST_ENABLE) part1 = i;
        if (writes[i].offset == EUR_CR_EVENT_HOST_ENABLE2) part2 = i;
        if (writes[i].offset == EUR_CR_EVENT_KICK2) kick = i;
        if (writes[i].offset == EUR_CR_SOFT_RESET) {
            if (first_reset == ~0U) first_reset = i;
            last_reset = i;
        }
    }
    assert(part1 < first_reset && last_reset < part2 && part2 < kick);

    setup(); dev.ui32CoreClockSpeed = 0;
    assert(SGXInitialise(&dev, IMG_TRUE) == PVRSRV_ERROR_INVALID_PARAMS);
    assert(!write_count && !waits_us && !kicks);

    /* The old infinite loop must return with reset asserted, no kick. */
    setup(); faults = ~0U;
    assert(SGXInitialise(&dev, IMG_TRUE) == PVRSRV_ERROR_TIMEOUT);
    assert(fault_reads == 65); assert_contained();

    /* Failure to initialize must stay a failure; simulated completion above
     * proves control flow only, never actual firmware compatibility. */
    setup(); init_complete = 0;
    assert(SGXInitialise(&dev, IMG_FALSE) == PVRSRV_ERROR_RETRY && kicks == 1);

#ifdef TEST_SINGLE_CONTEXT
    /* Propagate timeouts at initial drain, mapped-fault drain and final PD
     * invalidation, rather than logging and continuing into firmware. */
    for (i = 1; i <= 3; i++) {
        setup(); faults = 1; fail_poll = i;
        assert(SGXInitialise(&dev, IMG_TRUE) == PVRSRV_ERROR_TIMEOUT);
        assert(polls == i); assert_contained();
    }
#endif
    setup();
    dev.sScripts.asInitCommandsPart1[0].eOp = SGX_INIT_OP_ILLEGAL;
    assert(SGXInitialise(&dev, IMG_TRUE) == PVRSRV_ERROR_UNKNOWN_SCRIPT_OPERATION);
    assert(!kicks && !fault_reads);
    setup(); dev.sScripts.asInitCommandsPart2[0].eOp = SGX_INIT_OP_ILLEGAL;
    assert(SGXInitialise(&dev, IMG_TRUE) == PVRSRV_ERROR_UNKNOWN_SCRIPT_OPERATION && !kicks);
    /* Replay the user's real register inputs through actual native reset and
     * SGXInitialise, after policy normalization. Firmware completion remains
     * a simulated host-control write, never an execution of Intel firmware. */
    setup(); captured_replay = 1; faults=1;
    {
        struct gma_sgx_boot_plan plan;
        unsigned phase;
        assert(!gma_sgx_boot_plan_build(&captured_info, captured_memory, 15, &plan));
        dev.ui32ClkGateCtl = plan.info.clock_control[0];
        dev.ui32ClkGateCtl2 = plan.info.clock_control[1];
        for (phase = 0; phase < 2; phase++) {
            SGX_INIT_COMMAND *dst = phase ? dev.sScripts.asInitCommandsPart2 : dev.sScripts.asInitCommandsPart1;
            const struct sgx545_init_command32 *src = phase ? plan.info.scripts.part2 : plan.info.scripts.part1;
            for (i = 0; i < 64; i++) {
                dst[i].eOp = src[i].op == 2 ? SGX_INIT_OP_HALT : SGX_INIT_OP_WRITE_HW_REG;
                if (src[i].op == 2) break;
                dst[i].sWriteHWReg.ui32Offset = src[i].offset;
                dst[i].sWriteHWReg.ui32Value = src[i].value;
            }
            assert(i < 64);
        }
        assert(SGXInitialise(&dev, IMG_TRUE) == PVRSRV_OK && kicks == 1);
        assert(write_count >= 39);
        for (i = 0; i < 38; i++) {
            assert(writes[write_count - 39 + i].offset == plan.info.scripts.part2[i].offset);
            assert(writes[write_count - 39 + i].value == plan.info.scripts.part2[i].value);
        }
        assert(writes[write_count - 1].offset == EUR_CR_EVENT_KICK2);
    }
    puts("Captured D2500HN boot scripts replayed through actual native reset/start; host interrupts masked, original addresses preserved (simulated completion only)");
    puts("Native reset/start simulation: bounded fault drain, timeout propagation, PD setup, first kick and completion/failure passed (no hardware)");
    return 0;
}
