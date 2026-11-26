# Real Intel initializer and CPU handoff checkpoint

**Current versions: services 0.14.0 and private transport 0.6.0.** Rebuild both.
Keep the working `1.0.4-sgx-host2` display parent loaded.

The [native CPU cycle passed on the Atom](../research/sgx545/d2500hn-native-cycle.json),
both synthetically and with the real Intel initializer. All 15 CPU mappings
closed, 97,660 bytes remained unchanged, and unload reached zero pages and zero
blocked accesses. The main code buffer remained 53,096 bytes with 32,503 nonzero
bytes. This establishes CPU memory behavior, not GPU execution or firmware compatibility.

This checkpoint **retains the actual native CPU binding across ioctls**. A
file-owned lease holds native CCB bookkeeping and references to staged metadata.
The stage retains backing RAM and native PTEs. The existing single-call native
cycle still runs as a regression; then the transport calls BIND, QUERY, content
readback, UNBIND and ABORT. No ring offsets or event kicker are changed, and
no command is published or submitted.

The kernel requires a valid SGX handle, privileged stage-owner process, unchanged
staged contents, no CPU VMAs, valid native PTEs/metadata and an empty bounded CCB.
Each retained query rechecks those conditions and the live native pointer graph.
Only the binding's owning file can query or unbind it. Stage abort may retire it
from another authorized file in the same process. Disconnect, last file close
and auxiliary detach remove native references before freeing backing RAM/PTEs.

A failed BIND reply rolls the binding back while keeping the stage available.
A failed QUERY reply leaves the binding intact. UNBIND completes before its
reply, so a failed UNBIND copyout leaves the lease retired. Cleanup errors retain
staged RAM/PTEs for retry; they never leave a native pointer into freed RAM.
The new `native_bindings` and `native_errors` counters must finish at zero.

The binding saves native device state, restores it on retirement, and preserves
all accumulated MMU cache-invalidation bits, including bits added by cleanup of
existing heap mappings between calls. While bound, unrelated bridge operations,
MMU diagnostics and new CPU mappings return EBUSY. Close/disconnect cleanup can
still run; it cannot create or destroy the common MMU context. This deliberately
limits which native state may change during the lease. Initializer initialization
remains one-shot per services load.

GPU execution remains disabled. The full native initialization part 2 remains
blocked. No scripts, firmware, power/reset, IRQ or job submission execute.
Descriptors still lack native BM/ResMan registration and must not reach full
part 2 or native memory-free paths. Persistent CPU binding is lifetime groundwork
for that integration; hardware bring-up still needs power/reset, hardware page
walks, IRQ delivery, matching firmware and bounded failure recovery.

Intel mapping-release leases, alias/VMA lifetime handling and CPU content/PTE
checks remain in place. Userspace performs `munmap`; a metadata release never
frees device RAM or removes PTEs. Hashes detect changes; they do not authenticate firmware.

## Build on the Atom

From the repository root, use new output directories:

```sh
git pull --ff-only
bash tests/run.sh
sgx_build="$HOME/Desktop/gma3600-sgx-native-runtime-$(date +%Y%m%d-%H%M%S)"
python3 tools/sgx545_hybrid_build.py \
  --kdir "/lib/modules/$(uname -r)/build" \
  --source "$HOME/.cache/gma3600/openpvrsgx" \
  --sgx-rev 10131 --output "$sgx_build"
modinfo -F version "$sgx_build/core/gma3600_sgx_services.ko"
init_build="$HOME/Desktop/gma3600-initializer-native-runtime-$(date +%Y%m%d-%H%M%S)"
python3 tools/sgx545_initializer_build.py --output "$init_build"
python3 tools/sgx545_initializer_run.py --bundle "$init_build" --selftest
```

Expect services `0.14.0` and `transport=0.6.0`. The selftest must pass.
Older bundles do not run the retained binding cycle; build a new private bundle.
The builders do not install packages, load modules or execute proprietary
libraries. The initializer builder verifies checksum-pinned Intel and Ubuntu
i386 packages and extracts a private runtime, retaining `INTEL-LICENSE.txt`.
No old packages or libraries are installed into the system. The selftest
executes only the project-owned fixture. See the
[current validation record](../research/sgx545/native-runtime-6.12-build.json).

## Test retained native lifetime

Keep SSH connected and the working host2 parent loaded. Remove the old services
child or identification probe:

```sh
if [ -d /sys/module/gma3600_sgx_services ]; then
  sudo rmmod gma3600_sgx_services
fi
if [ -d /sys/module/gma3600_sgx_probe ]; then
  sudo rmmod gma3600_sgx_probe
fi
```

Run the explicit unbind/rebind/abort test:

```sh
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1
sudo "$sgx_build/sgx545-compat-probe" --native-runtime
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
```

Run the automatic last-close cleanup test on a fresh child:

```sh
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1
sudo "$sgx_build/sgx545-compat-probe" --native-runtime-close
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
```

Both modes exercise actual native binding, separate repeated queries, BIND
copyout rollback, QUERY reply-fault retention, busy guards, explicit unbind and
rebind. The first ends with stage abort while bound; the second closes the
owning file while bound. Expect native pointers to retire before staged RAM/PTEs.
Final status must show `native_bindings=0 native_errors=0`, four baseline pages,
and no other retained resources/errors/blocked accesses. Unload must reach zero
pages. Stop and return the output if any assertion fails.

Every initializer mode consumes the one-shot attempt. Reload the child between
modes and before the real capture. Older diagnostic modes remain available.

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

The runner requires the verified Atom PCI device, host2 parent, services 0.14.0,
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

The trace logs one `CONTENT` row per staged buffer, followed by
`CONTENT_SUMMARY checked=N buffers=N ... changed=0 errno=0 mapped=0`. A complete cycle
then ends with a line like:

```text
HANDOFF validated=1 staged=1 queried=1 aborted=1 buffers=N pages=N errno=0 gpu_execution=disabled
```

The JSON outcome is `native_runtime_verified_with_clean_resources` only
when the transient and retained native binding checks pass, every content row agrees with the
summary, the handoff/abort passes and resources are clean. The new
`native_cycle.verified` and `native_runtime.verified` must both be true. The new
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
files, connections, allocations, mappings, map_leases, stages, native_bindings, errors and blocked accesses.
Unload must report `result=0 pages_live=0 blocked_hw_accesses=0`.
Return the JSON at `$init_report`, regression output and cleanup lines. A repeat
capture needs a fresh services load and a new report filename.
