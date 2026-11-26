// SPDX-License-Identifier: GPL-2.0-only
/* CPU-only translation. Never calls the native initializer or executes scripts. */
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#define SGX545_FREESTANDING
#endif
#include "pvr_bridge.h"
#include "sgx_bridge.h"
#include "sgx_options.h"
#include "gma3600_sgx_init.h"

_Static_assert(sizeof(SGX_INIT_COMMAND) == 12, "tested SGX script format");
_Static_assert(sizeof(SGX_MISCINFO_STRUCT_SIZES) == 52, "tested SGX545 size vector");
_Static_assert(sizeof(IMG_HANDLE) == sizeof(void *), "native pointer handles required");

static void wire_roles(const struct sgx545_init2_info32 *in, __u32 roles[SGX545_INIT_ROLES])
{
	roles[0] = in->ccb; roles[1] = in->ccb_control; roles[2] = in->event_kicker;
	roles[3] = in->host_control; roles[4] = in->ta3d_control; roles[5] = in->misc;
	roles[6] = in->hwperf; roles[7] = in->ta_signature; roles[8] = in->signature_3d;
}

void gma_sgx_init_requirements(struct sgx545_init_requirements *out)
{
	unsigned int i;

	out->build_options = SGX_BUILD_OPTIONS;
	out->build_option_mask = SGX_BUILD_OPTION_MASK;
	for (i = 0; i < SGX545_INIT_ROLES; i++) out->minimum_bytes[i] = 4;
	out->minimum_bytes[0] = sizeof(PVRSRV_SGX_KERNEL_CCB);
	out->minimum_bytes[1] = sizeof(PVRSRV_SGX_CCB_CTL);
	out->minimum_bytes[3] = sizeof(SGXMKIF_HOST_CTL);
	out->minimum_bytes[5] = sizeof(PVRSRV_SGX_MISCINFO_INFO);
	out->minimum_bytes[6] = sizeof(SGXMKIF_HWPERF_CB);
	memset(out->struct_sizes, 0, sizeof(out->struct_sizes));
	/* Nine private structures still require the firmware-reported contract. */
	out->struct_sizes[1] = sizeof(SGXMKIF_CMDTA_SHARED);
	out->struct_sizes[3] = sizeof(SGXMKIF_TRANSFERCMD_SHARED);
	out->struct_sizes[11] = sizeof(SGXMKIF_HOST_CTL);
	out->struct_sizes[12] = sizeof(SGXMKIF_COMMAND);
}

int gma_sgx_init_tokens(const struct sgx545_init2_in32 *wire, __u32 *tokens, __u32 *count)
{
	__u32 roles[SGX545_INIT_ROLES], token, i, j, n = 0;

	*count = 0;
	if (wire->flags || !wire->device) return -EINVAL;
	wire_roles(&wire->info, roles);
	for (i = 0; i < SGX545_INIT_MAX_BUFFERS; i++) {
		token = i < SGX545_INIT_ROLES ? roles[i] : wire->info.mem_handles[i - SGX545_INIT_ROLES];
		if (!token) {
			if (i < SGX545_INIT_ROLES) return -EINVAL;
			continue;
		}
		for (j = 0; j < n; j++) if (tokens[j] == token) return -EINVAL;
		tokens[n++] = token;
	}
	*count = n;
	return 0;
}

static int translate_script(const struct sgx545_init_command32 *in, SGX_INIT_COMMAND *out,
	unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (in[i].op == 2) { out[i].eOp = SGX_INIT_OP_HALT; return 0; }
		if (in[i].op != 1) return -EINVAL;
		if (in[i].offset & 3) return -EINVAL;
		if (in[i].offset >= 0x4000) return -ERANGE;
		out[i].sWriteHWReg.eOp = SGX_INIT_OP_WRITE_HW_REG;
		out[i].sWriteHWReg.ui32Offset = in[i].offset;
		out[i].sWriteHWReg.ui32Value = in[i].value;
	}
	return -E2BIG;
}

static void native_roles(SGX_BRIDGE_INIT_INFO *info, IMG_HANDLE *roles[SGX545_INIT_ROLES])
{
	roles[0] = &info->hKernelCCBMemInfo; roles[1] = &info->hKernelCCBCtlMemInfo;
	roles[2] = &info->hKernelCCBEventKickerMemInfo; roles[3] = &info->hKernelSGXHostCtlMemInfo;
	roles[4] = &info->hKernelSGXTA3DCtlMemInfo; roles[5] = &info->hKernelSGXMiscMemInfo;
	roles[6] = &info->hKernelHWPerfCBMemInfo; roles[7] = &info->hKernelTASigBufferMemInfo;
	roles[8] = &info->hKernel3DSigBufferMemInfo;
}

int gma_sgx_init_translate(const struct sgx545_init2_in32 *wire,
	const struct gma_sgx_init_memory *memory, __u32 count, struct gma_sgx_native_init *out)
{
	struct sgx545_init_requirements caps = {0};
	const struct sgx545_init2_info32 *in = &wire->info;
	SGX_BRIDGE_INIT_INFO *info = &out->info;
	IMG_HANDLE *roles[SGX545_INIT_ROLES];
	__u32 tokens[SGX545_INIT_MAX_BUFFERS], expected, i, j;
	int ret;

	memset(out, 0, sizeof(*out));
	ret = gma_sgx_init_tokens(wire, tokens, &expected);
	if (ret) return ret;
	if (count != expected) return -EINVAL;
	gma_sgx_init_requirements(&caps);
	if ((in->build_options ^ caps.build_options) & caps.build_option_mask) return -EPROTO;
	for (i = 0; i < 13; i++) {
		if (caps.struct_sizes[i] && in->struct_sizes[i] != caps.struct_sizes[i]) return -EPROTO;
		if (in->struct_sizes[i] > SGX545_BUFFER_MAX_BYTES || (in->struct_sizes[i] & 3)) return -EINVAL;
	}
	if ((in->clock_status_reg & 3) || in->clock_status_reg >= 0x4000) return -ERANGE;
	native_roles(info, roles);
	for (i = 0; i < count; i++) {
		const struct gma_sgx_init_memory *m = &memory[i];
		PVRSRV_KERNEL_MEM_INFO *native = &out->memory[i];

		if (m->token != tokens[i] || !m->linear || !m->bytes || m->bytes > SGX545_BUFFER_MAX_BYTES ||
		    !m->address || (m->address & 4095) ||
		    (__u64)m->address + m->bytes > 0x100000000ULL ||
		    !sgx545_device_effective_flags_valid(m->flags)) { ret = -EINVAL; goto fail; }
		if (i < SGX545_INIT_ROLES && m->bytes < caps.minimum_bytes[i]) { ret = -ENOSPC; goto fail; }
		native->pvLinAddrKM = m->linear; native->sDevVAddr.uiAddr = m->address;
		native->ui32Flags = m->flags; native->uAllocSize = m->bytes;
		if (i < SGX545_INIT_ROLES) *roles[i] = native;
		else {
			for (j = 0; j < 18; j++) if (in->mem_handles[j] == m->token) break;
			info->asInitMemHandles[j] = native;
		}
	}
	ret = translate_script(in->scripts.part1, info->sScripts.asInitCommandsPart1, 64);
	if (!ret) ret = translate_script(in->scripts.part2, info->sScripts.asInitCommandsPart2, 64);
	if (!ret) ret = translate_script(in->scripts.deinit, info->sScripts.asDeinitCommands, 16);
	if (ret) goto fail;
	memcpy(info->aui32HostKickAddr, in->host_kick, sizeof(in->host_kick));
	info->ui32ClientBuildOptions = in->build_options;
	memcpy(&info->sSGXStructSizes, in->struct_sizes, sizeof(in->struct_sizes));
	info->ui32EDMTaskReg0 = in->edm_task[0]; info->ui32EDMTaskReg1 = in->edm_task[1];
	info->ui32ClkGateCtl = in->clock_control[0]; info->ui32ClkGateCtl2 = in->clock_control[1];
	info->ui32ClkGateStatusReg = in->clock_status_reg; info->ui32ClkGateStatusMask = in->clock_status_mask;
	info->ui32CacheControl = in->cache_control;
	memcpy(info->asInitDevData, in->dev_data, sizeof(in->dev_data));
	return 0;
fail:
	memset(out, 0, sizeof(*out));
	return ret;
}

int gma_sgx_init_check(const struct gma_sgx_native_init *snapshot,
	const struct gma_sgx_init_memory *memory, __u32 count)
{
	unsigned int i;

	if (count < SGX545_INIT_ROLES || count > SGX545_INIT_MAX_BUFFERS) return -EINVAL;
	for (i = 0; i < count; i++) {
		const PVRSRV_KERNEL_MEM_INFO *native = &snapshot->memory[i];
		if (native->pvLinAddrKM != memory[i].linear || native->sDevVAddr.uiAddr != memory[i].address ||
		    native->uAllocSize != memory[i].bytes || native->ui32Flags != memory[i].flags) return -EIO;
	}
	return 0;
}
