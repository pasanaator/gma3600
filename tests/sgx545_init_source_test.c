// SPDX-License-Identifier: GPL-2.0-only
/* Real DDK headers, native pointer handles, actual translator, mock addresses. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pvr_bridge.h"
#include "sgx_bridge.h"
#include "sgx_options.h"
#define SGX545_FREESTANDING
#include "gma3600_sgx_init.h"

_Static_assert(sizeof(void *) == 8 && sizeof(IMG_HANDLE) == 8, "real native pointer handles");
_Static_assert(sizeof(SGX_BRIDGE_INIT_INFO) == 2176, "matches measured amd64 kernel body");

static struct sgx545_init2_in32 wire;
static struct gma_sgx_init_memory memory[SGX545_INIT_MAX_BUFFERS];
static struct gma_sgx_native_init output;

static void setup(unsigned count)
{
    struct sgx545_init_requirements caps = {0};
    unsigned i, address = 0xf4000000;
    gma_sgx_init_requirements(&caps);
    memset(&wire, 0, sizeof(wire)); memset(memory, 0, sizeof(memory));
    wire.device = 7;
    wire.info.ccb = 100; wire.info.ccb_control = 101; wire.info.event_kicker = 102;
    wire.info.host_control = 103; wire.info.ta3d_control = 104; wire.info.misc = 105;
    wire.info.hwperf = 106; wire.info.ta_signature = 107; wire.info.signature_3d = 108;
    wire.info.build_options = caps.build_options;
    memcpy(wire.info.struct_sizes, caps.struct_sizes, sizeof(caps.struct_sizes));
    for (i = 0; i < 11; i++) wire.info.struct_sizes[i] = (i + 1) * 4;
    wire.info.scripts.part1[0] = (struct sgx545_init_command32){1, 0x80, 0x12345678};
    wire.info.scripts.part1[1].op = 2;
    wire.info.scripts.part1[63].op = 0xffffffff; /* Unused tail is normalized, not executed. */
    wire.info.scripts.part2[0].op = wire.info.scripts.deinit[0].op = 2;
    for (i = 0; i < count; i++) {
        unsigned size = i < 9 ? (caps.minimum_bytes[i] + 4095) & ~4095U : 4096;
        memory[i] = (struct gma_sgx_init_memory){100 + i, address, size,
            (SGX545_DEVICE_FLAGS & ~SGX545_DEVICE_ACCESS) | (i >= 9 ? i % 3 + 1 : SGX545_DEVICE_ACCESS) |
                SGX545_RAM_BACKED | SGX545_BUFFER_MULTI,
            (void *)(uintptr_t)(0x100000000ULL + i * 0x100000)};
        if (i >= 9) wire.info.mem_handles[i - 9] = 100 + i;
        address += size;
    }
    for (i = 0; i < 11; i++) wire.info.host_kick[i] = 0x98760000 + i;
    for (i = 0; i < 24; i++) wire.info.dev_data[i] = 0xabcd0000 + i;
    wire.info.edm_task[0] = 0x1234; wire.info.edm_task[1] = 0x5678;
    wire.info.clock_control[0] = 0x9abc; wire.info.clock_control[1] = 0xdef0;
    wire.info.clock_status_reg = 0x100; wire.info.clock_status_mask = 0x20;
    wire.info.cache_control = 0x30;
}
static void zero_output(void)
{
    unsigned i;
    for (i = 0; i < sizeof(output); i++) assert(!((unsigned char *)&output)[i]);
}

int main(void)
{
    unsigned count, i, tokens[27];
    setup(27);
    assert(!gma_sgx_init_tokens(&wire, tokens, &count) && count == 27);
    for (i = 0; i < count; i++) assert(tokens[i] == 100 + i);
    assert(!gma_sgx_init_translate(&wire, memory, count, &output));
    assert(output.info.hKernelCCBMemInfo == &output.memory[0]);
    assert(output.info.hKernelCCBCtlMemInfo == &output.memory[1]);
    assert(output.info.hKernelSGXHostCtlMemInfo == &output.memory[3]);
    assert(output.info.hKernelHWPerfCBMemInfo == &output.memory[6]);
    assert(output.info.hKernel3DSigBufferMemInfo == &output.memory[8]);
    for (i = 9; i < 27; i++) assert(output.info.asInitMemHandles[i - 9] == &output.memory[i]);
    for (i = 0; i < 27; i++) assert(output.memory[i].ui32Flags == memory[i].flags);
    assert(!gma_sgx_init_check(&output, memory, count));
    assert((uintptr_t)output.memory[0].pvLinAddrKM == 0x100000000ULL); /* no truncation */
    assert(!memcmp(output.info.aui32HostKickAddr, wire.info.host_kick, sizeof(wire.info.host_kick)));
    assert(!memcmp(output.info.asInitDevData, wire.info.dev_data, sizeof(wire.info.dev_data)));
    assert(!memcmp(&output.info.sSGXStructSizes, wire.info.struct_sizes, sizeof(wire.info.struct_sizes)));
    assert(output.info.sScripts.asInitCommandsPart1[0].sWriteHWReg.ui32Value == 0x12345678);
    assert(output.info.sScripts.asInitCommandsPart1[1].eOp == SGX_INIT_OP_HALT);
    assert(!output.info.sScripts.asInitCommandsPart1[63].eOp);
    assert(output.info.ui32EDMTaskReg0 == 0x1234 && output.info.ui32EDMTaskReg1 == 0x5678);
    assert(output.info.ui32ClkGateCtl == 0x9abc && output.info.ui32ClkGateCtl2 == 0xdef0);
    assert(output.info.ui32ClkGateStatusReg == 0x100 && output.info.ui32CacheControl == 0x30);
    output.memory[5].pvLinAddrKM = NULL;
    assert(gma_sgx_init_check(&output, memory, count) == -EIO);
    setup(9); memory[0].flags = SGX545_INTEL_INIT_FLAGS | SGX545_HEAP_FLAGS;
    assert(!gma_sgx_init_translate(&wire, memory, 9, &output));
    assert(output.memory[0].ui32Flags == memory[0].flags);
    assert(!gma_sgx_init_check(&output, memory, 9));
    memory[0].flags ^= SGX545_DEVICE_EDM_PROTECT;
    assert(gma_sgx_init_check(&output, memory, 9) == -EIO);

    setup(9); wire.info.build_options = 0x60218;
    assert(!gma_sgx_init_translate(&wire, memory, 9, &output));
    assert(output.info.ui32ClientBuildOptions == 0x60218); /* masked scheduler differences */
    setup(9); wire.info.ccb = 0;
    assert(gma_sgx_init_tokens(&wire, tokens, &count) == -EINVAL && !count);
    setup(9); wire.info.ccb_control = wire.info.ccb;
    assert(gma_sgx_init_tokens(&wire, tokens, &count) == -EINVAL);
    setup(9); wire.info.mem_handles[17] = wire.info.ccb;
    assert(gma_sgx_init_tokens(&wire, tokens, &count) == -EINVAL);
    setup(9); wire.info.build_options ^= 0x80;
    assert(gma_sgx_init_translate(&wire, memory, 9, &output) == -EPROTO); zero_output();
    setup(9); wire.info.struct_sizes[11]++;
    assert(gma_sgx_init_translate(&wire, memory, 9, &output) == -EPROTO); zero_output();
    setup(9); wire.info.struct_sizes[0] = 3;
    assert(gma_sgx_init_translate(&wire, memory, 9, &output) == -EINVAL); zero_output();
    setup(9); memory[0].bytes = 4;
    assert(gma_sgx_init_translate(&wire, memory, 9, &output) == -ENOSPC); zero_output();
    setup(9); memory[0].address = 0xfffff000;
    assert(gma_sgx_init_translate(&wire, memory, 9, &output) == -EINVAL); zero_output();
    setup(9); memory[8].token++;
    assert(gma_sgx_init_translate(&wire, memory, 9, &output) == -EINVAL); zero_output();
    setup(9); wire.info.scripts.part1[0].offset = 3;
    assert(gma_sgx_init_translate(&wire, memory, 9, &output) == -EINVAL); zero_output();
    setup(9); wire.info.scripts.part1[0].offset = 0x4000;
    assert(gma_sgx_init_translate(&wire, memory, 9, &output) == -ERANGE); zero_output();
    setup(9); wire.info.scripts.deinit[0].op = 0;
    assert(gma_sgx_init_translate(&wire, memory, 9, &output) == -EINVAL); zero_output();
    setup(9);
    for (i = 0; i < 64; i++) wire.info.scripts.part1[i] = (struct sgx545_init_command32){1, 0, 0};
    assert(gma_sgx_init_translate(&wire, memory, 9, &output) == -E2BIG); zero_output();
    setup(9); wire.info.clock_status_reg = 0x4000;
    assert(gma_sgx_init_translate(&wire, memory, 9, &output) == -ERANGE); zero_output();
    setup(9);
    assert(!gma_sgx_init_translate(&wire, memory, 9, &output));
    for (i = 0; i < 18; i++) assert(!output.info.asInitMemHandles[i]);
    puts("SGX initializer translation tests passed: real native DDK structures, 27 buffers, 64-bit pointers, bounded scripts and failure cleanup");
    return 0;
}
