/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef GMA3600_SGX_BRIDGE_H
#define GMA3600_SGX_BRIDGE_H
#include <linux/types.h>
struct device;
struct gma_sgx_bridge;
struct gma_sgx_bridge_stats {
	u32 files, connections, errors, heap_handles, init_running, init_ran;
	u32 buffers, buffer_pages, buffer_maps, buffer_errors, map_leases;
	u32 device_allocations, device_pages, device_errors;
	u32 stages, staged_buffers, staged_pages, stage_errors;
	u32 native_bindings, native_errors;
};
int gma3600_sgx_bridge_register(struct device *dev, struct gma_sgx_bridge **result);
void gma3600_sgx_bridge_stats(struct gma_sgx_bridge *bridge,
			     struct gma_sgx_bridge_stats *stats);
#endif
