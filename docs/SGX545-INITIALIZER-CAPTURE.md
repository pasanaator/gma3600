# Firmware startup preparation and Intel initializer capture

**The transport 0.9.0 boot-input capture has passed on the Atom.** Its ordered
scripts and original addresses are now covered by the offline policy/controller
and native source replay. See [first firmware boot](SGX545-FIRMWARE-BOOT.md) for
the audit, tested controller and remaining live ownership integration. No new
hardware capture or allocation-test loops are requested for this checkpoint.
The commands below remain available for future diagnostic captures.

Current private transport: **0.9.0**. Services **0.17.0** adds dormant native
reset fixes; **the already tested 0.16.0 child also supports this capture**.
Keep the working `1.0.4-sgx-host2` display parent loaded.

The [retained native allocator passed on the Atom](../research/sgx545/d2500hn-native-allocator.json):
all four synthetic modes, all 15 real initializer buffers / 36 pages, populated
contents, native binding, and zero-resource teardown. Do not repeat those loops
for this checkpoint.

This capture records every ordered register write supplied in `part1`, `part2`
and `deinit`, the EDM tasks, clock controls/status, host-kick entries and device
data. `startup_inputs.complete` means the trace is complete and structurally
valid; **it does not approve any register or value for execution**. The report
includes a fingerprint for comparison. Some addresses and data can vary between
runs, so the fingerprint is not an accepted-firmware whitelist.

Services 0.17.0 caps the legacy reset fault-drain loop at 64 attempts and returns
invalidation failures to `SGXInitialise`, stopping it before the first kick.
It also rejects missing core-clock timing before any startup register access;
the legacy clock-wait helper would otherwise divide by zero.
The production prepare-only MMIO gate remains closed. The complete native reset
helpers, clock-wait arithmetic and native `SGXInitialise` first-kick path are
source-tested with mocked MMIO, delay and completion under ASan/UBSan for both SGX545 profiles. This checks
software control flow; it does not verify GPU page walks or firmware execution.

## Capture on the Atom using the existing child build

Run from the repository root. `$sgx_build` must identify the existing build
containing services 0.16.0 (or a new 0.17.0 build). No parent reload is needed.

```sh
git pull --ff-only
modinfo -F version "$sgx_build/core/gma3600_sgx_services.ko"
init_build="$HOME/Desktop/gma3600-firmware-inputs-$(date +%Y%m%d-%H%M%S)"
python3 tools/sgx545_initializer_build.py --output "$init_build"
python3 tools/sgx545_initializer_run.py --bundle "$init_build" --selftest
```

Expect `transport=0.9.0` and a passing project-owned fixture. Then use a fresh
services instance; initializer attempts are deliberately one-shot:

```sh
for module in gma3600_sgx_services gma3600_sgx_probe; do
  if [ -d "/sys/module/$module" ]; then
    sudo rmmod "$module" || break
  fi
done
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1 native_bm=1
sudo python3 tools/sgx545_initializer_run.py \
  --bundle "$init_build" --capture --report "$init_build/initializer-report.json"
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
```

Send `initializer-report.json`. Expected outcome remains
`native_allocator_verified_with_clean_resources`, plus
`startup_inputs.complete=true` and ordered script writes in the report.
The legacy initializer is still deliberately returned a failure after the CPU
handoff cycle; there is no firmware startup in this capture. CPU content and
ownership checks still run automatically during capture.

No package is installed or system library replaced. The initializer bundle
keeps the original Intel libraries unchanged, together with their license and
checksum-pinned private i386 runtime. Its selftest executes project-owned code.
Real proprietary execution occurs only in `--capture` on the supported Atom.

## First live firmware milestone

The next hardware milestone is a bounded `INIT_COMPLETE` response from the
microkernel, followed by its build/revision/structure-size reply. It is separate
from rendering or attaching the desktop to an accelerated driver.

Before that attempt:

1. Review the captured scripts against the pinned SGX545 register definitions,
   including values and ordering. An offset below `0x4000` alone is insufficient.
   Mask SGX host interrupts during the initial polling-only experiment.
2. Add a dedicated boot owner for the retained native allocation set, native CPU
   binding, MMU directory, reset pages and device state. Exclude competing bridge
   operations and suspend/detach while hardware owns these resources.
3. Establish parent-coordinated SGX power/reset handling without resetting the
   display PCI function. The old Cedarview power callbacks are empty and are not
   evidence of an isolated, working power-off path.
4. Install only the verified original page directory, execute reviewed startup
   writes, publish WC RAM with barriers, issue one `EVENT_KICK2`, and poll the host
   control status with a fixed deadline. Keep all render submissions disabled.
5. On success, request the firmware's compatibility information. Nine private
   structure layouts still require firmware-reported confirmation. On failure,
   record status/fault registers and establish quiescence before releasing RAM.
   A reset assertion or userspace timeout alone is not proof that DMA stopped;
   unresolved ownership must retain/quarantine every GPU-visible page.

See [build validation](../research/sgx545/firmware-startup-6.12-build.json).

## Retained allocator details

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
