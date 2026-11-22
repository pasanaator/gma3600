// SPDX-License-Identifier: GPL-2.0-only
/* Link-check adapter for the separated DDK services. Runtime comes later. */
#include <linux/module.h>
#include <linux/pci.h>
#include <drm/drm_device.h>
#include "services_headers.h"
#include "linkage.h"
#include "pvr_bridge.h"
#include "sgx545_abi.h"
#include "sgxdefs.h"

/* Compile against the real pinned DDK types, not a re-created native layout. */
static_assert(sizeof(PVRSRV_BRIDGE_PACKAGE) == 48);
static_assert(offsetof(PVRSRV_BRIDGE_PACKAGE, hParamOut) == 24);
static_assert(sizeof(struct sgx545_bridge32) == 28);
static_assert(EUR_CR_CORE_ID == 0x001c);
static_assert(EUR_CR_CORE_REVISION == 0x0020);

struct pci_dev *gpsPVRLDMDev;
struct drm_device *gpsPVRDRMDev;

struct device *PVRLDMGetDevice(void)
{
	return gpsPVRLDMDev ? &gpsPVRLDMDev->dev : NULL;
}

static int __init gma3600_sgx_init(void)
{
	pr_err("gma3600 SGX core: link validation only; runtime ABI/memory/power port incomplete\n");
	return -EOPNOTSUPP;
}
module_init(gma3600_sgx_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GMA3600 hybrid SGX services link check (no hardware access)");
MODULE_VERSION("0.1.0");
