# Real Intel initializer and CPU handoff checkpoint

**Current versions: services 0.13.0 and private transport 0.5.0.** Rebuild both.
Keep the working `1.0.4-sgx-host2` display parent loaded.

The [latest Atom result](../research/sgx545/d2500hn-mapping-release.json) passed
all 15 Intel CPU mapping releases. All staged CPU VMAs had closed; 97,660 logical
bytes remained unchanged, including 32,815 nonzero bytes in nine buffers.
Native PTEs/snapshots and teardown passed. The main 53,096-byte GPU code buffer
contained 32,503 nonzero bytes. These results establish CPU memory and lifetime
behavior; they do not authenticate firmware or establish GPU execution.

This checkpoint calls the **actual native `InitDevInfo` CPU prefix** against the
live native device, under the bridge/global DDK locks, then restores it before
returning. The temporary native CCB metadata points at staged buffers. The test
writes 32 bytes through the actual native command-slot pointer, reads them back,
restores the original bytes, and calls `DeinitDevInfo`. It restores all native
device fields, including preexisting MMU cache-control state. The native cleanup
routine now frees from the same heap used by allocation and clears its pointer.
Allocation-failure paths also restore the device. No native pointers escape the
transaction. No command-ring offset or event kicker is changed.

The kernel requires no active CPU VMAs, unchanged baseline hashes, valid native
PTEs/metadata, GPU read/write access for the nine runtime buffers, and a bounded
empty command ring. It checks all staged content and PTEs before and after.
The native source fixture exercises the actual DDK bodies for both profiles,
including slot 0/255, failures of both allocations and exact device restoration.

The private transport runs `VALIDATE`, `STAGE`, `QUERY`, **`NATIVE_CYCLE`**, content
readback and `ABORT`. It always attempts abort after staging, including a native
cycle failure. The legacy initializer is deliberately returned a failure.

GPU execution remains disabled. The full native initialization part 2 stays
blocked; no scripts, firmware, power/reset, IRQ or job submission execute.
Descriptors are still private snapshots, not registered native BM/ResMan objects,
and must not be passed to full native part 2 or native memory-free paths.
Persistent native ownership, native memory/resource integration, power/reset,
hardware page walks, IRQ delivery and firmware compatibility remain to be done.

Intel command 27, `RELEASE_MMAP_DATA`, remains supported with counted metadata
leases and independent mapping groups. Userspace performs `munmap`; a lease
release never removes GPU PTEs or frees backing RAM. Failed reply copies roll back
lease changes. Partial canonical ranges and canonical moves are rejected;
independent raw aliases and split VMA references preserve their own lifetimes.
The mmap callback returns retryable EAGAIN on bridge contention to avoid an
mmap-lock/ioctl-copyout lock inversion. Checksums are change detectors only.

## Build on the Atom

From the repository root, use new output directories:

```sh
git pull --ff-only
bash tests/run.sh
sgx_build="$HOME/Desktop/gma3600-sgx-native-cycle-$(date +%Y%m%d-%H%M%S)"
python3 tools/sgx545_hybrid_build.py \
  --kdir "/lib/modules/$(uname -r)/build" \
  --source "$HOME/.cache/gma3600/openpvrsgx" \
  --sgx-rev 10131 --output "$sgx_build"
modinfo -F version "$sgx_build/core/gma3600_sgx_services.ko"
init_build="$HOME/Desktop/gma3600-initializer-native-cycle-$(date +%Y%m%d-%H%M%S)"
python3 tools/sgx545_initializer_build.py --output "$init_build"
python3 tools/sgx545_initializer_run.py --bundle "$init_build" --selftest
```

Expect services `0.13.0` and `transport=0.5.0`. The selftest must pass.
Older bundles do not run the native CPU cycle; build a new private bundle.
The builders do not install packages, load modules or execute proprietary
libraries. The initializer builder verifies checksum-pinned Intel and Ubuntu
i386 packages and extracts a private runtime, retaining `INTEL-LICENSE.txt`.
No old packages or libraries are installed into the system. The selftest
executes only the project-owned fixture. See the
[current validation record](../research/sgx545/native-cycle-6.12-build.json).

## Test the native CPU cycle

Keep SSH connected and the working host2 parent loaded. Remove the old services
child or identification probe, then run the new synthetic test:

```sh
if [ -d /sys/module/gma3600_sgx_services ]; then
  sudo rmmod gma3600_sgx_services
fi
if [ -d /sys/module/gma3600_sgx_probe ]; then
  sudo rmmod gma3600_sgx_probe
fi
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1
sudo "$sgx_build/sgx545-compat-probe" --native-cycle
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
```

Expect the synthetic content/staging checks and actual native CPU cycle to pass,
including state restoration after an ioctl reply-copy fault, unchanged contents
and explicit abort. Final status must have four baseline pages and zero retained
resources/errors/blocked accesses. Unload must reach zero pages.
Stop and return the output if a check fails.

Each initializer mode consumes a one-shot attempt, so reload the child between
modes. Existing `--map-release`, `--buffers`, `--init-stage` and
`--init-stage-close` modes remain available; the latter intentionally retains
staged pages until unload.

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

The runner requires the verified Atom PCI device, host2 parent, services 0.13.0,
fresh prepare-only state, root privileges and a new report filename. It verifies
bundle hashes and uses a private library path with a 25-second timeout. It does
not acquire or release DRM/KMS master, so the working desktop remains on llvmpipe.

A successful native cycle logs:

```text
NATIVE bound=1 restored=1 ccb_bytes=8192 slots=256 read=0 write=0 roundtrip=1 buffers=15 pages=36 gpu_execution=disabled
```

The trace logs one `CONTENT` row per staged buffer, followed by
`CONTENT_SUMMARY checked=N buffers=N ... changed=0 errno=0 mapped=0`. A complete cycle
then ends with a line like:

```text
HANDOFF validated=1 staged=1 queried=1 aborted=1 buffers=N pages=N errno=0 gpu_execution=disabled
```

The JSON outcome is `native_cpu_cycle_verified_with_clean_resources` only
when the native bind/restore/CCB checks pass, every content row agrees with the
summary, the handoff/abort passes and resources are clean. The new
`native_cycle.verified` must be true. The new
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
files, connections, allocations, mappings, map_leases, stages, errors and blocked accesses.
Unload must report `result=0 pages_live=0 blocked_hw_accesses=0`.
Return the JSON at `$init_report`, regression output and cleanup lines. A repeat
capture needs a fresh services load and a new report filename.
