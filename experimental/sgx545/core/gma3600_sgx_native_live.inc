/* SPDX-License-Identifier: GPL-2.0-only */
/* Included beside the audited CPU binder, not through legacy part 2/power.
 * No ISR, power device, recovery timer or render submission is registered. */
#include "sysinfo.h"
#include "sgxmmu.h"
#include "gma3600_sgx_live_native.h"
#if !defined(SGX545) || defined(SGX_FEATURE_MP) || !defined(SGX_FEATURE_MULTIPLE_MEM_CONTEXTS)
#error First live provider requires the pinned non-MP SGX545 multiple-context profile
#endif
_Static_assert(SGX_MMU_PDE_ADDR_ALIGNSHIFT == 0, "first live trial uses the pinned 32-bit physical MMU profile");
_Static_assert(EUR_CR_SOFT_RESET == 0x80 && EUR_CR_BIF_INT_STAT == 0xc04 && EUR_CR_BIF_FAULT == 0xc08, "live register policy");
_Static_assert(EUR_CR_BIF_DIR_LIST_BASE0 == 0xc84 && EUR_CR_BIF_DIR_LIST_BASE1 == 0xc38 && SGX_BIF_DIR_LIST_INDEX_EDM == 15, "live directory policy");
_Static_assert(EUR_CR_EVENT_STATUS == 0x12c && EUR_CR_EVENT_STATUS2 == 0x118 && EUR_CR_BIF_INT_STAT_FAULT_REQ_MASK == 0xffff, "live diagnostics policy");

int gma3600_sgx_native_live_prepare(void *runtime, void *registers, struct gma_sgx_live_roots *roots)
{
	struct gma_sgx_native_runtime_state *b = runtime;
	struct sgx545_native_cycle state = {0};
	PVRSRV_SGXDEV_INFO *d;
	int ret = gma3600_sgx_native_query(runtime, &state);
	if (ret) return ret;
#if SGX_CORE_REV != 10131
	return -EOPNOTSUPP; /* Secondary build profile is not the first hardware trial. */
#endif
	d = b->dev;
	if (!roots || !registers || (d->pvRegsBaseKM && d->pvRegsBaseKM != registers) ||
	    state.read_offset || state.write_offset ||
	    !d->sKernelPDDevPAddr.uiAddr || !d->sBIFResetPDDevPAddr.uiAddr ||
	    !d->sBIFResetPTDevPAddr.uiAddr || !d->sBIFResetPageDevPAddr.uiAddr ||
	    !d->pui32BIFResetPD || !d->pui32BIFResetPT) return -EINVAL;
	if ((d->sKernelPDDevPAddr.uiAddr | d->sBIFResetPDDevPAddr.uiAddr |
	     d->sBIFResetPTDevPAddr.uiAddr | d->sBIFResetPageDevPAddr.uiAddr) & 4095) return -EINVAL;
	/* Part 1 leaves the native register pointer NULL. Legacy part 2 normally
	 * assigns it, but that entry point stays blocked. Borrow only the base
	 * supplied by the held parent lease; this neither maps nor accesses MMIO.
	 * Include it in binding integrity checks. Pre-seal unbind restores the
	 * saved device image (and NULL pointer); sealing forbids that teardown.
	 */
	d->pvRegsBaseKM = registers;
	b->expected.pvRegsBaseKM = registers;
	/* Pinned Cedarview platform timing. SGXWaitClocks needs this before reset;
	 * changing the CPU copy does not program a frequency or enable a timer. */
	d->ui32CoreClockSpeed = SYS_SGX_CLOCK_SPEED;
	d->ui32uKernelTimerClock = SYS_SGX_CLOCK_SPEED / SYS_SGX_PDS_TIMER_FREQ;
	b->expected.ui32CoreClockSpeed = d->ui32CoreClockSpeed;
	b->expected.ui32uKernelTimerClock = d->ui32uKernelTimerClock;
	roots->kernel_pd = d->sKernelPDDevPAddr.uiAddr >> SGX_MMU_PDE_ADDR_ALIGNSHIFT;
	roots->reset_pd = d->sBIFResetPDDevPAddr.uiAddr;
	roots->edm_dir = SGX_BIF_DIR_LIST_INDEX_EDM == 0 ? EUR_CR_BIF_DIR_LIST_BASE0 :
		EUR_CR_BIF_DIR_LIST_BASE1 + 4 * (SGX_BIF_DIR_LIST_INDEX_EDM - 1);
	roots->bank = SGX_BIF_DIR_LIST_INDEX_EDM << EUR_CR_BIF_BANK0_INDEX_EDM_SHIFT;
#if defined(FIX_HW_BRN_23410)
	roots->bank |= SGX_BIF_DIR_LIST_INDEX_EDM << EUR_CR_BIF_BANK0_INDEX_TA_SHIFT;
#endif
#if defined(SGX_FEATURE_36BIT_MMU)
	roots->mmu36 = 1;
#else
	roots->mmu36 = 0;
#endif
	return 0;
}

int gma3600_sgx_native_live_arm(void *runtime)
{
	struct gma_sgx_native_runtime_state *b = runtime;
	struct sgx545_native_cycle state = {0};
	PVRSRV_SGXDEV_INFO *d;
	int ret = gma3600_sgx_native_query(runtime, &state);
	if (ret) return ret;
	if (state.read_offset || state.write_offset || !b->dev->ui32CoreClockSpeed) return -EINVAL;
	d = b->dev;
	b->hardware_owned = 1; /* Never restore/free this binding after sealing. */
	OSMemSet(d->psKernelCCB, 0, sizeof(*d->psKernelCCB));
	OSMemSet(d->psKernelCCBCtl, 0, sizeof(*d->psKernelCCBCtl));
	*d->pui32KernelCCBEventKicker = 0;
	d->psSGXHostCtl->ui32PowerStatus = PVRSRV_USSE_EDM_POWMAN_NO_WORK;
	d->psSGXHostCtl->ui32ActivePowManSampleRate = 0; /* No asynchronous power requests. */
	d->psSGXHostCtl->ui32HWRecoverySampleRate = SYS_SGX_PDS_TIMER_FREQ / SYS_SGX_HWRECOVERY_TIMEOUT_FREQ;
	d->psSGXHostCtl->ui32uKernelTimerClock = d->ui32uKernelTimerClock;
	d->psSGXHostCtl->ui32InitStatus = 0;
	OSMemoryBarrier();
	return 0;
}

int gma3600_sgx_native_live_reset(void *runtime, __u32 *pvr_error)
{
	struct gma_sgx_native_runtime_state *b = runtime;
	PVRSRV_ERROR error;
	if (!b || !b->hardware_owned || !pvr_error) return -EINVAL;
	/* The actual native reset is bounded by patch 0013 and the MMIO gate. */
	error = SGXReset(b->dev, IMG_TRUE, 0);
	*pvr_error = error;
	return error == PVRSRV_OK ? 0 : -ETIMEDOUT;
}
int gma3600_sgx_native_live_publish(void *runtime)
{
	struct gma_sgx_native_runtime_state *b = runtime;
	if (!b || !b->hardware_owned) return -EINVAL;
	b->dev->psSGXHostCtl->ui32HostClock = OSClockus();
	b->dev->psSGXHostCtl->ui32InitStatus = 0;
	OSMemoryBarrier();
	return 0;
}
int gma3600_sgx_native_live_status(void *runtime, __u32 *status)
{
	struct gma_sgx_native_runtime_state *b = runtime;
	if (!b || !b->hardware_owned || !status) return -EINVAL;
	OSMemoryBarrier();
	*status = b->dev->psSGXHostCtl->ui32InitStatus;
	return 0;
}
