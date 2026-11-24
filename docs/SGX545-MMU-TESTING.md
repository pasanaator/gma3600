# Test the native SGX MMU routines without GPU execution

**Hardware result:** all ten 0.7.0 cases and cleanup passed on the D2500HN; see
[the recorded transcript results](../research/sgx545/d2500hn-offline-mmu.json).
The next test is [persistent device memory](SGX545-DEVICE-MEMORY-TESTING.md).
These historical 0.7.0 instructions remain useful for that checkpoint; current
builds report the newer services version.

The [shared-buffer tests passed on the Atom](../research/sgx545/d2500hn-shared-buffers.json),
including user/kernel WC coherence and both mapping cleanup paths. Services
**0.7.0** adds a bounded **offline page-table test** using the real DDK allocator,
scatter mapper, translation lookup and free routines.

It reserves device addresses in the three existing shared initializer heaps,
maps four actual buffer pages in descending physical-address order, checks valid
PTEs and CPU translation readback, then frees the reservation and page tables
before returning. Larger heaps also exercise a mapping across a 4 MiB page-table
boundary and release an adjacent VA reservation while the PTEs remain live.

The reported `expired_va` is already unmapped. This is CPU inspection of SGX
page-table structures in RAM. **The driver never binds that directory to the GPU
or runs a GPU memory access.** Hardware DMA/MMU behavior, firmware matching and
rendering remain unvalidated; llvmpipe is still expected.

The patch also fixes a legacy bookkeeping bug: individual page-table frees must
preserve the heap's total PTE capacity. Host tests reproduce the failure against
the old routine and verify repeated frees, partial final tables and PT allocation
rollback with the fix. Empty metadata/pages left by failed native MMU allocation
are reclaimed before this synchronous diagnostic returns. WC table writes have
explicit memory barriers before readback and page release.

## Build on the Atom

From the repository root, using the pinned source cache from previous builds:

```sh
git pull --ff-only
bash tests/run.sh
sgx_build="$HOME/Desktop/gma3600-sgx-mmu1"
python3 tools/sgx545_hybrid_build.py \
  --kdir /lib/modules/$(uname -r)/build \
  --source "$HOME/.cache/gma3600/openpvrsgx" \
  --sgx-rev 10131 \
  --output "$sgx_build"
sudo modinfo -F version "$sgx_build/core/gma3600_sgx_services.ko"
"$sgx_build/sgx545-compat-probe" --selftest
```

The output directory must be new. Module version must be `0.7.0`; selftest now
includes `mmu_check=40`. The builder also compiles and runs the actual patched DDK
MMU functions with mock OS/RA allocation, for both supported compile profiles.
Those checks do not execute GPU hardware. No proprietary/i386 libraries are needed.

## Run one fresh initializer attempt

Keep SSH connected and stream `sudo dmesg -wH` in another terminal. Reuse the
loaded `1.0.4-sgx-host2` parent. If absent, follow the existing
[late-load procedure](../experimental/sgx545/README.md#test-identification-on-the-d2500hn)
using the host module in this build directory.

```sh
cat /sys/module/gma3600/version
if [ -d /sys/module/gma3600_sgx_services ]; then sudo rmmod gma3600_sgx_services; fi
if [ -d /sys/module/gma3600_sgx_probe ]; then sudo rmmod gma3600_sgx_probe; fi
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo "$sgx_build/sgx545-compat-probe" --mmu
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
sudo dmesg | grep -Ei 'gma3600|PVR_K|blocked hardware|BUG:|Oops' | tail -80
```

Run `--mmu` alone on this load. It includes the ordinary bridge/shared-buffer
checks and consumes the one-shot initializer, so reload the child before any
other initializer mode or repeat attempt.

Expected diagnostics resemble:

```text
MMU heap=0x07000002 expired_va=0xf2000000 pages=4 pt_peak=1 pt_after=0 scatter_breaks=3
MMU heap=0x07000003 expired_va=0xf43fe000 pages=4 pt_peak=2 pt_after=0 scatter_breaks=3
PASS: native DDK offline MMU scatter mappings, PT boundary crossings, repeated free and zero remaining page tables
PASS: i386 shared buffers, zeroed padding, user/kernel WC roundtrip, map isolation, VMA splits and free/disconnect with live mappings
```

Each supported case runs twice to check reuse. There should be three shared
heaps; the larger two support the boundary case. Normal cases report one PT and
boundary cases two; all cases must return `pt_after=0`, four checked pages and
three noncontiguous scatter transitions. Buffer contents must remain unchanged
through these temporary mappings. The client also checks invalid tokens/flags
and an output-copy fault after the native mapping has already been cleaned up.

Final status must have `pages_live=4`, `blocked_hw_accesses=0`, `init_running=0`,
`init_ran=1`, and zero file, connection, heap-handle, buffer, mapping and error
counters. Cleanup must again report:

```text
gma3600 SGX buffers stopped: buffers=0 pages=0 maps=0 errors=0
gma3600 SGX bridge stopped: files=0 connections=0 errors=0 heap_handles=0 init_running=0 init_ran=1
services cleanup: result=0 pages_live=0 blocked_hw_accesses=0
```

Return the MMU/client output, status/cleanup lines, any unexpected PVR errors,
and whether the desktop stayed usable. The deliberately invalid device-index
test may log `PVRSRVAcquireDeviceDataKM: requested device is not present`; that
message followed by the normal device PASS is expected. On failure, capture
status/logs and unload the child after the client exits. Stop if cleanup retains
pages or any blocked hardware access is reported.

This diagnostic only accepts four pages and the tested 4 KiB SGX545 shared-heap
configuration. It refuses heaps with existing PT metadata, local-memory heaps,
MMU bypass, dummy-page or BRN31620 configurations. Persistent device allocations,
the complete legacy memory ABI, DMA ownership, bounded power/reset/IRQ handling
and firmware compatibility remain prerequisites for hardware GLES rendering.
