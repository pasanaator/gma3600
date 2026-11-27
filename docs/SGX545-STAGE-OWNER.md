# Stage and parent ownership checkpoint

Historical CPU checkpoint, now [passed on the Atom](../research/sgx545/d2500hn-stage-owner.json).
For the current child and explicit live test use [firmware boot](SGX545-FIRMWARE-BOOT.md).

Versions: **host 1.0.4-sgx-host3, services 0.18.0, transport 0.10.0**.
The previous Atom software run passed all 13 C suites and 31 Python tests.
This checkpoint adds actual resource ownership; it does not start firmware.

## Behavior

The 48-byte privileged `BOOT_OWNER` ioctl has ACQUIRE, QUERY and RELEASE only.
It requires a retained native CPU binding, the original native BM allocations,
released CPU mappings, unchanged contents and the captured boot recipe. The
sanitized plan remains attached to its stage while the lease is held.

Acquisition pins both modules, the auxiliary device, parent PCI device and DRM
storage. Parent locking serializes lease acquisition against suspend, remove
and SGX IRQ registration. Runtime PM is pinned only if already active, or
without resume when disabled; obtaining a lease never wakes hardware.
System suspend is refused before touching the display when
a lease exists. Failed suspend rolls back the transition; failed display resume
continues to exclude new leases until restoration succeeds.

While leased, explicit native unbind and native BM alias diagnostics are busy.
Abort, disconnect, last-close and forced child detach release the CPU lease
before native binding/PTE/backing teardown. Failed acquisition/copyout rolls
back references. Failed QUERY copyout retains the existing lease; failed RELEASE
copyout does not recreate a released lease. Failed parent retirement retains
the stage, bridge, pool, common DDK state and all references. Late close does
not enter dead native services. These failures must not be forced through.
Device references retain storage, not permission to use MMIO after detach.

ABI 3 also has an internal one-way seal for the eventual live boot provider.
Only the verified Intel 8086:0be1 device in D0 can seal its own existing lease.
After seal, release returns busy permanently, including during forced remove;
there is no unseal API. This gives the first future hardware attempt a
reboot-only resource lifetime. No current ioctl or parameter invokes seal,
changes SGX power, writes GPU registers, installs a PD or kicks firmware.

The original native binding keeps its original script image for integrity
checks. A live provider must execute the sanitized `boot_plan` through the boot
controller and supply valid native timing and bounded reset/status hooks. It
must not jump into legacy part 2 or execute the original unmasked script.

The new child and identification probe can still use ABI 2's original prefix.
They do not read its missing tail. The new capture runner requires host3 because
its ownership phase needs the new lease operations.

## Build on the Atom

Use new directories. The fixed names below make it easy to recover the paths
after a reboot; choose different names if they already exist.

```sh
git pull --ff-only
bash tests/run.sh
sgx_build="$HOME/Desktop/gma3600-sgx-owner18"
python3 tools/sgx545_hybrid_build.py \
  --kdir "/lib/modules/$(uname -r)/build" \
  --source "$HOME/.cache/gma3600/openpvrsgx" \
  --sgx-rev 10131 --output "$sgx_build"
init_build="${sgx_build}-initializer"
python3 tools/sgx545_initializer_build.py --output "$init_build"
python3 tools/sgx545_initializer_run.py --bundle "$init_build" --selftest
```

Expect the project-owned selftest to pass with transport 0.10.0. No packages are
installed and the builders do not execute the proprietary initializer.

## Load the new parent and test once

This needs a fresh host3 parent load. Use SSH and your usual baseline boot where
you manually load gma3600. If `/sys/module/gma3600` already exists, keep that
working display and reboot into the baseline before proceeding. Do not force
unload the display parent. After reboot, return to the repository directory:

```sh
sgx_build="$HOME/Desktop/gma3600-sgx-owner18"
init_build="${sgx_build}-initializer"
sudo modprobe -a drm_kms_helper drm_ttm_helper drm_display_helper i2c_algo_bit
sudo insmod "$sgx_build/host/gma3600.ko" refclk=27000 sgx_host=1
cat /sys/module/gma3600/version
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1 native_bm=1
cat /sys/module/gma3600_sgx_services/version
sudo python3 tools/sgx545_initializer_run.py \
  --bundle "$init_build" --capture --report "$init_build/initializer-report.json"
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
sudo dmesg | grep -Ei 'SGX|services cleanup|BUG:|Oops|WARNING:' | tail -20
```

Do not load the separate identification probe at the same time. Send the new
`initializer-report.json`, service status and unload log. Expected outcome is
`boot_owner_verified_with_clean_resources`, with acquired/queried/released all
1, parent ABI 3, 15 buffers / 36 pages, and no remaining owner/lease/error counts.
Unload should report `result=0 pages_live=0 blocked_hw_accesses=0`. If unload
reports busy, leave the modules in place and send the status; do not force it.

The desktop should continue to work as before with llvmpipe. There is no need to
repeat allocation-test loops or test suspend/PCI unbind on the Atom. Those paths
are fault/race-tested with simulated resources in the cloud.

See [validation](../research/sgx545/stage-owner-6.12-build.json) for the exact
scope and the remaining live-provider work.
