# Real Intel initializer and CPU handoff checkpoint

**Current versions: services 0.16.0 and private transport 0.8.0.** Rebuild both.
Keep the working `1.0.4-sgx-host2` display parent loaded.

The [native BM alias tests passed on the Atom](../research/sgx545/d2500hn-native-bm.json),
including all 15 real Intel buffers / 36 pages, two wrap/free/reuse cycles each,
unchanged populated contents and zero-page unloads.

This checkpoint adds an **opt-in native device-allocation backend**. Load the
services child with `native_bm=1`; it remains fixed for that services instance.
The default remains the prior MMU allocator. The capture runner now requires
`allocator=native_bm` before executing the real initializer.

The modern provider supplies zeroed, pinned WC pages and manages CPU VMAs.
Native BM wraps those pages at allocation time and owns the original GPU VA,
PTEs and kernel-only OS registration. The original request's logical byte count
and normalized power-of-two alignment are preserved. The native wrapping path
now takes alignment explicitly, including 64 KiB; the legacy public wrapper
retains its page-alignment behavior. Addresses are assigned before userspace
populates buffers, so addresses embedded by Intel's initializer name the retained
native allocation rather than a later alias.

Staging borrows real BM/OS handles into its native meminfo snapshots. Validation
checks those handles, native getters, CPU-order physical pages and PTE attributes.
The snapshots remain bridge-owned: legacy ResMan, reference-count and free
contracts are not fabricated. Do not pass them to legacy native memory-free
paths or full initialization part 2.

The file/stage owns the BM object and backing pin. Native CPU binding retires
before native BM/OS ownership; native PTE retirement is verified before dropping
the backing pin. User VMAs can retain their WC pages after native ownership is
freed. Reply faults, disconnect, last-close and auxiliary detach retain those
orders. Failed retirement quarantines the object and original RAM pin rather
than retrying a released reservation. The new live counters `bm_allocations`
and `bm_pages` must reach zero after cleanup.

The new `BM_INFO` ioctl returns opaque metadata for each original allocation:
GPU address, logical size, page count, requested alignment and proof of retained
BM/OS/snapshot attachment. It exposes no native pointers. During capture the
transport checks ownership for every staged buffer, runs the earlier temporary
alias tests, rechecks the retained native CPU binding and buffer contents, then
unbinds and aborts. `bm_checks` remains a cumulative diagnostic count.

Four public firmware structures remain independently compiler-audited and
enforced: CMDTA_SHARED=480, TRANSFERCMD_SHARED=220, HOST_CTL=68 and COMMAND=32.
Nine private structures need future firmware-reported confirmation. GPU execution
remains disabled: no power/reset, scripts, IRQ or firmware/jobs are executed.
Hardware DMA/page walks, firmware compatibility, command/resource integration
and bounded startup/recovery still need implementation and validation.

## Build on the Atom

From the repository root, use new output directories:

```sh
git pull --ff-only
bash tests/run.sh
sgx_build="$HOME/Desktop/gma3600-sgx-native-allocator-$(date +%Y%m%d-%H%M%S)"
python3 tools/sgx545_hybrid_build.py \
  --kdir "/lib/modules/$(uname -r)/build" \
  --source "$HOME/.cache/gma3600/openpvrsgx" \
  --sgx-rev 10131 --output "$sgx_build"
modinfo -F version "$sgx_build/core/gma3600_sgx_services.ko"
init_build="$HOME/Desktop/gma3600-initializer-native-allocator-$(date +%Y%m%d-%H%M%S)"
python3 tools/sgx545_initializer_build.py --output "$init_build"
python3 tools/sgx545_initializer_run.py --bundle "$init_build" --selftest
```

Expect services `0.16.0` and `transport=0.8.0`. The selftest must pass.
Older bundles do not verify original retained BM ownership; build a new private bundle.
The builders do not install packages, load modules or execute proprietary
libraries. The initializer builder verifies checksum-pinned Intel and Ubuntu
i386 packages and extracts a private runtime, retaining `INTEL-LICENSE.txt`.
No old packages or libraries are installed into the system. The selftest
executes only the project-owned fixture. See the
[current validation record](../research/sgx545/native-allocator-6.12-build.json).

## Test native allocation and lifetime

Keep SSH connected and the working host2 parent loaded. Remove the old child
or identification probe:

```sh
for module in gma3600_sgx_services gma3600_sgx_probe; do
  if [ -d "/sys/module/$module" ]; then
    sudo rmmod "$module"
  fi
done
```

Run each mode on a fresh child with the new allocator selected:

```sh
for mode in --device-intel --device-close --native-alloc --native-alloc-close; do
  sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1 native_bm=1 || break
  sudo "$sgx_build/sgx545-compat-probe" "$mode" || break
  cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
  sudo rmmod gma3600_sgx_services || break
done
```

Stop if a mode fails. The device tests exercise original/compact payloads,
64 KiB alignment, sibling allocation lifetime, CPU WC roundtrip and free or
close with live VMAs. The allocator tests add staging, retained native binding,
original BM/OS/snapshot ownership, temporary aliases, reply-fault handling and
explicit or automatic last-close abort. Expect `allocator=native_bm`,
`bm_allocations=0 bm_pages=0 bm_errors=0 native_bindings=0 native_errors=0`, four
baseline pages and no retained buffers/stages/mappings. The two staging modes
also report `bm_checks=12`; the device modes need no alias checks. Unload must
reach zero pages. Earlier diagnostic modes and the default allocator remain available.

Every initializer mode consumes the one-shot attempt. Reload the child before
real capture.

## Capture the unmodified Intel initializer

Reload once more, then capture using the new private bundle:

```sh
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1 native_bm=1
init_report="$init_build/initializer-report.json"
sudo python3 tools/sgx545_initializer_run.py \
  --bundle "$init_build" --capture --report "$init_report"
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
sudo dmesg | grep -Ei 'SGX buffers|SGX bridge|services cleanup|blocked hardware|BUG:|Oops' | tail -10
```

The runner requires the verified Atom PCI device, host2 parent, services 0.16.0,
fresh prepare-only state with `native_bm=1`, root privileges and a new report filename. It verifies
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

Original ownership must log one `OWNED` row per staged buffer and:

```text
OWNED_SUMMARY checked=15 buffers=15 pages=36 errno=0 allocator=native_bm
```

Each row must have `owned=1 registered=1 attached=1`, matching original GPU
address/content metadata and a valid preserved alignment. The capture additionally logs one `BM` row per buffer and:

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

The JSON outcome is `native_allocator_verified_with_clean_resources` only
when native BM checks, the transient and retained native binding checks pass, every content row agrees with the
summary, the handoff/abort passes and resources are clean. The new
`native_cycle.verified`, `native_runtime.verified`, `native_bm.verified` and
`native_ownership.verified` must all be true. The new
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
files, connections, allocations, mappings, map_leases, stages, native_bindings, bm_allocations, bm_pages, errors and blocked accesses. `bm_checks=15` is expected
for the real capture, while `bm_errors=0` is required.
Unload must report `result=0 pages_live=0 blocked_hw_accesses=0`.
Return the JSON at `$init_report`, regression output and cleanup lines. A repeat
capture needs a fresh services load and a new report filename.
