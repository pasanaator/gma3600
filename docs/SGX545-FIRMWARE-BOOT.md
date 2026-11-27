# First SGX545 firmware boot

Versions: **host 1.0.4-sgx-host3 / services 0.19.1 / transport 0.11.0**.
The [CPU stage/parent ownership test](../research/sgx545/d2500hn-stage-owner.json)
passed on the D2500HN, including clean teardown of 15 original allocations / 36
pages. This next checkpoint connects that owner to a real polling boot provider.
The [first 0.19.0 attempt](../research/sgx545/d2500hn-live-preflight.json)
stopped before sealing with `EINVAL`: zero writes, zero polls, clean resources.
The prepared native device had no register pointer because the skipped legacy
part 2 normally assigned it. Version **0.19.1** borrows the held parent's mapping
into the retained native device image; pre-seal cleanup restores the original
NULL field. It still rejects a foreign non-NULL mapping. The corrected first
hardware sequence awaits a new Atom attempt; firmware has not run yet.

## What this attempts

Both the module parameter `firmware_boot=1` and client flag `--firmware-boot`
are required for the documented hardware test. Without the client flag the
original CPU checks still run, abort and return to idle. The kernel parameter
alone never boots firmware. The privileged bridge independently checks every
request, the existing native binding, released CPU mappings, actual PTEs and
page-directory identity, original BM ownership, content and the strict script
recipe. There is no generic register-write or render-command interface.

The live provider acquires the host3 lease, establishes the pinned Cedarview
400 MHz timing values in native CPU state, then permanently seals the lease
before the first GPU register write. It uses clock/interrupt controls, the
validated part-1 script, the actual bounded native `SGXReset` (including original
PD/reset pages), sanitized part 2, host-control/WC barriers, one `EVENT_KICK2`,
and up to 500 ms of shared-memory polling for `INIT_COMPLETE`. Native reset MMIO
is allowed only for this task, base and documented reset register/value set,
within the same deadline and a 4096-access cap. Other native accesses remain
blocked. A software deadline cannot interrupt a stuck bus transaction.

Only profile **10131** can enter the first live trial; 10141 remains build/source
tested and rejects the trial before sealing. The revision token and firmware
ABI are not established by a successful build. Even `INIT_COMPLETE` will still
need firmware build/revision and private structure-size replies before rendering.

The [register audit](../research/sgx545/d2500hn-boot-register-audit.json) compares
both public header sets: 19 of 41 distinct script offsets remain unnamed in both.
Those writes are restricted to the exact observed Intel recipe; their full
semantics and display coexistence are not yet established. The provider does
not change PCI power or display registers. It masks both SGX-local interrupt
banks, including replacing the script's `EVENT_HOST_ENABLE=0x4000` with zero.
It does not register the old ISR, power device or recovery timer, or call full
native part 2. Neither this checkpoint nor its success enables 3D acceleration.

## Resource lifetime and results

A sealed attempt retains the original firmware/CCB/host-control pages, PTEs,
page directory, reset pages, native binding, stage, native services, both module
pins, parent PCI/DRM storage and runtime-PM lease **until reboot**. There is no
unseal or retry API. Normal unload and system suspend are blocked while sealed.
The display may go blank during this first experiment; use SSH to collect logs.

On failure the provider records BIF/event status, masks interrupts and asserts
SGX reset when the device is accessible. Reset assertion is containment, not
proof of quiescence, so it never authorizes freeing pages. Lost-device/all-ones
reads stop subsequent MMIO; detach performs no MMIO and retains the owner.
Success also retains everything. Do not use forced unload, PCI unbind, suspend
or another boot attempt to recover; reboot after collecting the evidence.

The 64-byte private `LIVE_BOOT` START/QUERY message reports a saved result. A
lost START copyout may already have started firmware: the client only queries
and never retries START, unbinds native state or aborts the stage afterward.
The kernel also publishes cached `live_*` sysfs fields and a dmesg summary.
Services 0.19.1 append `live_phase`: 0 validate, 1 acquire, 2 verify, 3 clocks,
4 part1, 5 reset, 6 part2, 7 publish, 8 kick, 9 poll. The runner records its name.
The transport stays **0.11.0** and the existing private bundle can be reused.
`state=2`, completion bit 0 set, zero error/reset error and no BIF fault indicate
the first completion milestone. `state=4` means resources are quarantined after
a failure. Positive `error` values are Linux errno numbers; `reset_error` is the
native PVR error code. Neither QUERY nor sysfs rereads hardware.

The report distinguishes `firmware_init_complete_resources_retained`,
`firmware_boot_failed_resources_retained`, failures before sealing and unknown
results. It checks retained ownership separately; `resources_clean=false` is
expected once sealed. A legacy initializer failure is still intentional: this
private first boot does not advertise complete legacy initialization success.
The desktop continues to use llvmpipe.

## Build on the Atom

Use a fresh directory; these commands install no package or module:

```sh
git pull --ff-only
bash tests/run.sh
sgx_build="$HOME/Desktop/gma3600-sgx-live191-$(date +%Y%m%d-%H%M%S)"
python3 tools/sgx545_hybrid_build.py \
  --kdir "/lib/modules/$(uname -r)/build" \
  --source "$HOME/.cache/gma3600/openpvrsgx" \
  --sgx-rev 10131 --output "$sgx_build"
init_build="${sgx_build}-initializer"
python3 tools/sgx545_initializer_build.py --output "$init_build"
python3 tools/sgx545_initializer_run.py --bundle "$init_build" --selftest
```

Expect transport 0.11.0 and a passing project-owned selftest. The builder only
extracts/hashes the pinned packages; selftest does not execute Intel code.

The 0.19.0 result above was unsealed and all live counters returned to zero,
so that particular idle child can be replaced without rebooting. An already
sealed child requires reboot; never force its removal.

## One live attempt over SSH

Reuse your already working **host3** display parent. Do not unload it. The first
command below must show `1.0.4-sgx-host3`; if it does not, use your previous
baseline boot and load the host3 module first. Stop on any load/unload error.
Only an idle CPU-only child/probe can be unloaded in the following block:

```sh
cat /sys/module/gma3600/version
if [ -d /sys/module/gma3600_sgx_services ]; then
  sudo rmmod gma3600_sgx_services
fi
if [ -d /sys/module/gma3600_sgx_probe ]; then
  sudo rmmod gma3600_sgx_probe
fi
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" \
  prepare=1 bridge=1 native_bm=1 firmware_boot=1
cat /sys/module/gma3600_sgx_services/version
sudo python3 tools/sgx545_initializer_run.py \
  --bundle "$init_build" --capture --firmware-boot \
  --report "$init_build/firmware-boot-report.json"
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services \
  | tee "$init_build/services-after.txt"
sudo dmesg > "$init_build/kernel-after.log"
```

Expect child 0.19.1. Send `firmware-boot-report.json`, `services-after.txt` and
`kernel-after.log`, and describe whether VGA/desktop stayed responsive. Then
reboot. **Do not rmmod or rerun capture after the live attempt**, including
on timeout, a failed report copy or an unexpected result. A pending JSON report
also records that a hardware attempt may have occurred.

## Software validation

See [preflight-fix builds](../research/sgx545/live-preflight-fix-6.12-build.json)
and [original live-provider validation](../research/sgx545/live-boot-6.12-build.json).
Actual provider/controller tests simulate completion, every controller write
failure, timeout, faults, scoped reset access, wrong base/thread/interrupt,
lost-device MMIO exclusion and sealed retention. The actual native reset,
MMU root, CPU binding and host-control helpers have separate source tests
against real DDK types, including both build profiles. Bridge lifetime tests
cover copyout faults, close, detach and retained backing/common context. The
i386 transport tests cover lost START/QUERY replies and no destructive cleanup.
No kernel module or proprietary initializer is executed in the cloud.
