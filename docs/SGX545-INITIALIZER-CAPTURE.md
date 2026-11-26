# Real Intel initializer and CPU handoff checkpoint

**Current versions: services 0.15.0 and private transport 0.7.0.** Rebuild both.
Keep the working `1.0.4-sgx-host2` display parent loaded.

The [retained native binding tests passed on the Atom](../research/sgx545/d2500hn-native-runtime.json),
including explicit abort, last-close cleanup and the real Intel initializer.
All 15 buffers / 36 pages / 97,660 logical bytes remained unchanged; unload
reached zero pages and zero blocked accesses.

This checkpoint exercises the **actual native buffer manager** against the
existing pinned WC backing pages. A restricted entry calls native `WrapMemory`,
which registers a kernel-only external-memory handle, obtains a fresh native
GPU VA and maps scatter pages through the native device callbacks. It verifies
CPU/OS/GPU handle getters, per-page physical addresses and PTE access/coherency/EDM
attributes, then calls native `BM_Free`. Page-table retirement and capacity are
checked; a second cycle verifies VA reuse. The original allocation, GPU address
and contents must survive both cycles.

The public `BM_Wrap` policy inherits user mapping flags and drops coherency/EDM
flags, so this restricted constructor calls its underlying `WrapMemory` with
explicit kernel-only/WC/native PTE attributes. It creates no legacy user mmap
registration. A discovered failure-path bug is fixed: failed OS registration
must not attempt to unregister the caller's CPU address with a NULL OS handle.

These are temporary aliases, not an allocator replacement. No BM handle or OS
registration survives the call. If alias PTE retirement cannot be proven, the
stage and original RAM pin are quarantined rather than released. Other errors
are recorded in `bm_errors`; successful checks increment `bm_checks`. A reply
copy fault occurs after aliases have already been retired, so repeating the
check is safe. `bm_checks` is a cumulative diagnostic count, not a live resource.

The real transport retains the native CPU binding while it checks all staged
buffers with BM, then queries the native binding again, reads contents, unbinds
and aborts the stage. MMU invalidation flags accumulated during BM cleanup survive
unbind. Existing ownership, no-live-CPU-VMA and content/PTE checks remain enforced.

Four public firmware structures now have independent original-Intel i386,
compatibility-i386 and actual amd64 compiler measurements, including relevant
shared-command field offsets:

| Initializer index | Structure | Bytes |
| --- | --- | ---: |
| 1 | CMDTA_SHARED | 480 |
| 3 | TRANSFERCMD_SHARED | 220 |
| 11 | HOST_CTL | 68 |
| 12 | COMMAND | 32 |

All four sizes are enforced during staging. Nine other structures are private
and unavailable in the public kernel source; their actual firmware-reported
contract still needs verification. The ABI metadata schema is now version 3;
rebuild the core before running the ABI audit helper.

GPU execution remains disabled. No firmware, power/reset, IRQ, script or job
submission executes. Current snapshot descriptors are not retained BM/ResMan
allocations and must not reach full native part 2 or native memory-free paths.
Allocator integration, ResMan/DMA ownership, power/reset, hardware page walks,
IRQ delivery, firmware compatibility and bounded recovery remain to be done.
Hashes detect content changes; they do not authenticate firmware.

## Build on the Atom

From the repository root, use new output directories:

```sh
git pull --ff-only
bash tests/run.sh
sgx_build="$HOME/Desktop/gma3600-sgx-native-bm-$(date +%Y%m%d-%H%M%S)"
python3 tools/sgx545_hybrid_build.py \
  --kdir "/lib/modules/$(uname -r)/build" \
  --source "$HOME/.cache/gma3600/openpvrsgx" \
  --sgx-rev 10131 --output "$sgx_build"
modinfo -F version "$sgx_build/core/gma3600_sgx_services.ko"
init_build="$HOME/Desktop/gma3600-initializer-native-bm-$(date +%Y%m%d-%H%M%S)"
python3 tools/sgx545_initializer_build.py --output "$init_build"
python3 tools/sgx545_initializer_run.py --bundle "$init_build" --selftest
```

Expect services `0.15.0` and `transport=0.7.0`. The selftest must pass.
Older bundles do not run the native BM cycle; build a new private bundle.
The builders do not install packages, load modules or execute proprietary
libraries. The initializer builder verifies checksum-pinned Intel and Ubuntu
i386 packages and extracts a private runtime, retaining `INTEL-LICENSE.txt`.
No old packages or libraries are installed into the system. The selftest
executes only the project-owned fixture. See the
[current validation record](../research/sgx545/native-bm-6.12-build.json).

## Test the native buffer manager

Keep SSH connected and the working host2 parent loaded. Remove the old services
child or identification probe:

```sh
if [ -d /sys/module/gma3600_sgx_services ]; then
  sudo rmmod gma3600_sgx_services
fi
if [ -d /sys/module/gma3600_sgx_probe ]; then
  sudo rmmod gma3600_sgx_probe
fi
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1
sudo "$sgx_build/sgx545-compat-probe" --native-bm
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
```

The test includes the previous staging and retained-binding regressions, then
native BM checks on all 11 synthetic buffers, including read-only GPU code.
It also forces a BM result-copy fault and repeats the check to verify retirement.
Expect `bm_checks=12 bm_errors=0`, `native_bindings=0 native_errors=0`, four baseline
pages and no retained buffers, stages, mappings or errors. Unload must reach zero
pages. Stop and return the output if an assertion fails.

Every initializer mode consumes the one-shot attempt. Reload the child before
the real capture. Earlier diagnostic modes remain available.

## Capture the unmodified Intel initializer

Reload once more, then capture using the new private bundle:

```sh
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1
init_report="$init_build/initializer-report.json"
sudo python3 tools/sgx545_initializer_run.py \
  --bundle "$init_build" --capture --report "$init_report"
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
sudo dmesg | grep -Ei 'SGX buffers|SGX bridge|services cleanup|blocked hardware|BUG:|Oops' | tail -10
```

The runner requires the verified Atom PCI device, host2 parent, services 0.15.0,
fresh prepare-only state, root privileges and a new report filename. It verifies
bundle hashes and uses a private library path with a 25-second timeout. It does
not acquire or release DRM/KMS master, so the working desktop remains on llvmpipe.

A successful native cycle logs:

```text
NATIVE bound=1 restored=1 ccb_bytes=8192 slots=256 read=0 write=0 roundtrip=1 buffers=15 pages=36 gpu_execution=disabled
```

The retained cycle must also log:

```text
RUNTIME bound=1 queried=1 unbound=1 buffers=15 pages=36 errno=0 gpu_execution=disabled
```

The capture additionally logs one `BM` row per buffer and:

```text
BM_SUMMARY checked=15 buffers=15 pages=36 errno=0 gpu_execution=disabled
```

Each BM row must have `passes=2`, equal page-table counts before/after and an
alias different from the original address. A retained native QUERY runs again
after all BM checks. The trace logs one `CONTENT` row per staged buffer, followed by
`CONTENT_SUMMARY checked=N buffers=N ... changed=0 errno=0 mapped=0`. A complete cycle
then ends with a line like:

```text
HANDOFF validated=1 staged=1 queried=1 aborted=1 buffers=N pages=N errno=0 gpu_execution=disabled
```

The JSON outcome is `native_bm_verified_with_clean_resources` only
when native BM checks, the transient and retained native binding checks pass, every content row agrees with the
summary, the handoff/abort passes and resources are clean. The new
`native_cycle.verified`, `native_runtime.verified` and `native_bm.verified` must all be true. The new
`mapping_releases.all_staged_cpu_mappings_released` must be true.
`contents.buffers` holds the measured metadata and
`contents.summary` reports populated bytes/buffers. Missing or changed contents
cannot be reported as verified.
The Intel initializer still returns failure deliberately. If it stops earlier,
the JSON records the first unsupported request and may have `handoff: null`.
The updated runner also writes `first_failed_request`, including allocation
size, flags and alignment when available, and prints that failure separately
from a deliberately stopped successful CPU handoff cycle.
A failed handoff includes its progress and errno; crashes, timeouts, unexpected
initializer success or leftover resources produce `inspect_report`.

After process exit expect `pages_live=4`, `gpu_execution=disabled`, and zero
files, connections, allocations, mappings, map_leases, stages, native_bindings, errors and blocked accesses. `bm_checks=15` is expected
for the real capture, while `bm_errors=0` is required.
Unload must report `result=0 pages_live=0 blocked_hw_accesses=0`.
Return the JSON at `$init_report`, regression output and cleanup lines. A repeat
capture needs a fresh services load and a new report filename.
