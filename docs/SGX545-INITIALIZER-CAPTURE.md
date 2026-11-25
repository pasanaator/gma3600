# Real Intel initializer and CPU handoff checkpoint

**Current versions: services 0.10.0 and private transport 0.2.0.** Rebuild both;
the older transport 0.1.4 bundle cannot exercise the new handoff path. Keep the
working `1.0.4-sgx-host2` display parent loaded.

The [latest Atom trace](../research/sgx545/d2500hn-explicit-multi.json) confirms
that the real initializer allocated its 4-byte/alignment=16 buffer and cleaned
up correctly. It next requested 236 bytes aligned to 32 with flags `0x2004f`.
The additional flag is Intel's `PVRSRV_HAP_MULTI_PROCESS`, already present in
the supported heap attributes and successful allocation replies. This release
accepts an explicit request for that attribute. Mapping, queries and freeing
still require the owning file/process; cross-process import/export is not
implemented. CPU aliases remain WC, and the existing access/coherent/EDM PTE
policy, allocation bounds and zeroed page backing are retained.

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
sgx_build="$HOME/Desktop/gma3600-sgx-handoff-$(date +%Y%m%d-%H%M%S)"
python3 tools/sgx545_hybrid_build.py \
  --kdir "/lib/modules/$(uname -r)/build" \
  --source "$HOME/.cache/gma3600/openpvrsgx" \
  --sgx-rev 10131 --output "$sgx_build"
modinfo -F version "$sgx_build/core/gma3600_sgx_services.ko"
init_build="$HOME/Desktop/gma3600-sgx-initializer-handoff-$(date +%Y%m%d-%H%M%S)"
python3 tools/sgx545_initializer_build.py --output "$init_build"
python3 tools/sgx545_initializer_run.py --bundle "$init_build" --selftest
```

Expect services `0.10.0` and `transport=0.2.0`. The selftest must pass.
The builders do not install packages, load modules or execute proprietary
libraries. The initializer builder verifies checksum-pinned Intel and Ubuntu
i386 packages and extracts a private runtime, retaining `INTEL-LICENSE.txt`.
No old packages or libraries are installed into the system. The selftest
executes only the project-owned fixture. See the
[current validation record](../research/sgx545/handoff-6.12-build.json).

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
sudo "$sgx_build/sgx545-compat-probe" --device-intel
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
```

Expect the exact 236-byte/alignment=32/flags=0x2004f test to pass, followed by
all three heaps, PTE attributes, neighboring allocations, WC roundtrip and
foreign-file mapping/free rejection. All resource/error counters must return
to zero, with four baseline pages before unload and zero afterward.

Each initializer mode consumes a one-shot attempt, so reload between modes:

```sh
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1
sudo "$sgx_build/sgx545-compat-probe" --init-stage
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
```

The synthetic staging test now also checks repeated validation and failed
validation-copyout without moving buffer ownership. It then tests staging
copyout rollback, creator close, same-process control and explicit abort.
Stop and return the logs if either regression test fails.

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

The runner requires the verified Atom PCI device, host2 parent, services 0.10.0,
fresh prepare-only state, root privileges and a new report filename. It verifies
bundle hashes and uses a private library path with a 25-second timeout. It does
not acquire or release DRM/KMS master, so the working desktop remains on llvmpipe.

If the CPU handoff cycle passes, expect a line like:

```text
HANDOFF validated=1 staged=1 queried=1 aborted=1 buffers=N pages=N errno=0 gpu_execution=disabled
```

The JSON outcome is then `cpu_handoff_cycle_passed_with_clean_resources`.
The Intel initializer still returns failure deliberately. If it stops earlier,
the JSON records the first unsupported request and may have `handoff: null`.
A failed handoff includes its progress and errno; crashes, timeouts, unexpected
initializer success or leftover resources produce `inspect_report`.

After process exit expect `pages_live=4`, `gpu_execution=disabled`, and zero
files, connections, allocations, mappings, stages, errors and blocked accesses.
Unload must report `result=0 pages_live=0 blocked_hw_accesses=0`.
Return the JSON at `$init_report`, regression output and cleanup lines. A repeat
capture needs a fresh services load and a new report filename.
