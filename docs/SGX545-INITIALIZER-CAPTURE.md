# Real Intel initializer and CPU handoff checkpoint

**Current versions: services 0.12.0 and private transport 0.4.0.** Rebuild both.
Keep the working `1.0.4-sgx-host2` display parent loaded.

The [latest Atom result](../research/sgx545/d2500hn-buffer-contents.json) verified
all 15 staged buffers: 97,660 logical bytes, 32,815 nonzero bytes in nine buffers,
no changed data, zero padding and clean teardown. The main 53,096-byte GPU code
allocation contained 32,503 nonzero bytes. Synthetic CPU writes/change/restore
also passed. These are CPU memory results, not firmware authentication or GPU
execution.

This checkpoint implements Intel command 27, `RELEASE_MMAP_DATA`. MAP_INFO now
holds a counted metadata lease and returns the existing canonical CPU address
on reuse. A nonfinal release returns `unmap=0`; the final release returns only
that canonical mapping's address and rounded size. **Userspace performs munmap.**
The command does not free RAM or remove GPU PTEs. Failed MAP_INFO/release reply
copies roll back the lease operation. Handle free, disconnect and detach retire
remaining metadata leases; the new `map_leases` counter must end at zero.

Each mmap has a separate mapping group, so raw aliases retain independent VMA
references. VMA splits share a group. Partial closes make the canonical full
range uncertain: metadata reuse/release then returns EBUSY until it is entirely
unmapped, after which release returns no range. Moving canonical mappings with
mremap is rejected; temporary destinations created during a rejected move do
not invalidate the original range. Additional raw aliases remain independent.
Callers must serialize their own address-space changes with lease operations.

The mmap callback now uses a bridge trylock. It returns retryable EAGAIN during
contention instead of waiting while holding mmap_lock against an ioctl reply
fault. This prevents that lock-order inversion without weakening ownership.

Transport 0.4.0 forwards the real release calls and logs their returned ranges.
Content rows now include `mapped=0/1`, measured from actual live VMAs. A passing
capture requires one successful final release for each of the known Intel
initializer's 15 buffers, no remaining CPU mappings at staging, unchanged data,
valid native PTEs and clean abort. Thus the next hardware result will establish
that staged RAM survives actual legacy CPU unmapping, rather than only process
exit. Checksums remain FNV-1a change detectors, not authentication.

GPU execution remains disabled. Reset/power/clock work, native memory/resource
integration, GPU page walks, IRQ handling and firmware startup remain separate
bring-up stages.

This checkpoint also connects the real initializer's final handoff to the
existing CPU staging implementation:

1. The private transport copies the 2,068-byte Intel command 99 payload and
   normalizes its reserved first word in the copy. It logs buffer-role tokens,
   required minimum sizes, supplied/required structure sizes and script-format
   metadata. It never logs firmware bytes or register-write values.
2. A new project-only `INIT_VALIDATE` ioctl checks ownership, mapped PTEs,
   allocation sizes, build options, structure sizes and bounded scripts using
   the same translator as staging. Validation changes no allocation owner,
   token sequence, initializer state or retained-resource count.
3. If validation passes, the transport calls `STAGE`, `QUERY`, then `ABORT`.
   It verifies the reported counts and attempts abort even if query fails.
   Successful staging ends the one-shot initializer attempt in the kernel;
   the later legacy abort acknowledges that already-performed operation.
4. The legacy command still returns failure. No script, firmware, native
   initialization part 2, reset, power operation or GPU job is executed.

The kernel's legacy command 99 remains blocked. Native staging snapshots are
private metadata, not registered native BM/resource-manager allocations.
This checkpoint tests the handoff's CPU data and lifetime contract; it does
not establish firmware compatibility, GPU DMA or rendering. The real
initializer may encounter another missing allocation/service interface before
handoff, or its payload may fail the strict checks. Return that trace rather
than changing the checks to fit it.

## Build on the Atom

From the repository root, use new output directories:

```sh
git pull --ff-only
bash tests/run.sh
sgx_build="$HOME/Desktop/gma3600-sgx-map-release-$(date +%Y%m%d-%H%M%S)"
python3 tools/sgx545_hybrid_build.py \
  --kdir "/lib/modules/$(uname -r)/build" \
  --source "$HOME/.cache/gma3600/openpvrsgx" \
  --sgx-rev 10131 --output "$sgx_build"
modinfo -F version "$sgx_build/core/gma3600_sgx_services.ko"
init_build="$HOME/Desktop/gma3600-initializer-map-release-$(date +%Y%m%d-%H%M%S)"
python3 tools/sgx545_initializer_build.py --output "$init_build"
python3 tools/sgx545_initializer_run.py --bundle "$init_build" --selftest
```

Expect services `0.12.0` and `transport=0.4.0`. The selftest must pass.
The older 0.3.0 bundle blocks command 27 and lacks mapping-state readback; build a new private bundle.
The builders do not install packages, load modules or execute proprietary
libraries. The initializer builder verifies checksum-pinned Intel and Ubuntu
i386 packages and extracts a private runtime, retaining `INTEL-LICENSE.txt`.
No old packages or libraries are installed into the system. The selftest
executes only the project-owned fixture. See the
[current validation record](../research/sgx545/map-release-6.12-build.json).

## Allocation and staging regressions

Keep SSH connected and the working host2 parent loaded. Remove the old services
child or identification probe, then run the updated allocation test:

```sh
if [ -d /sys/module/gma3600_sgx_services ]; then
  sudo rmmod gma3600_sgx_services
fi
if [ -d /sys/module/gma3600_sgx_probe ]; then
  sudo rmmod gma3600_sgx_probe
fi
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1
sudo "$sgx_build/sgx545-compat-probe" --map-release
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
```

Expect the mapping test to pass canonical-address reuse, duplicate-reference
release, MAP_INFO/release copyout rollback, independent aliases, rejected
canonical moves, fragmented-range rejection and free with a live raw alias.
Stop and return the log if any assertion fails. The final status must show
`map_leases=0`, zero buffers/maps/errors and four baseline pages before unload.

Each initializer mode consumes a one-shot attempt, so reload between modes:

```sh
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1
sudo "$sgx_build/sgx545-compat-probe" --buffers
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
```

The shared-buffer regression checks the prior two-alias, WC roundtrip,
VMA-split, free/disconnect and deferred mapping lifetimes against the new mapping
groups. Stop if it fails. The existing `--init-stage` content test is also still
supported; it intentionally retains CPU mappings while testing writes.

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

The runner requires the verified Atom PCI device, host2 parent, services 0.12.0,
fresh prepare-only state, root privileges and a new report filename. It verifies
bundle hashes and uses a private library path with a 25-second timeout. It does
not acquire or release DRM/KMS master, so the working desktop remains on llvmpipe.

The trace logs one `CONTENT` row per staged buffer, followed by
`CONTENT_SUMMARY checked=N buffers=N ... changed=0 errno=0 mapped=0`. A complete cycle
then ends with a line like:

```text
HANDOFF validated=1 staged=1 queried=1 aborted=1 buffers=N pages=N errno=0 gpu_execution=disabled
```

The JSON outcome is `cpu_handoff_content_verified_with_clean_resources` only
when every content row agrees with the summary, the handoff/abort passes and
resources are clean. The new
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
