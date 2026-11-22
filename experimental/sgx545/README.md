# SGX545 hybrid port for Debian 13 amd64

This is the first implementation stage of the selected hybrid port. It builds
against Debian `6.12.117+deb13-amd64`. Hardware 3D is **not enabled yet**.
The first rendering target is an i386 EGL/GLES2 offscreen process on the amd64
kernel. The available Intel libraries do not accelerate amd64 applications.
The [build validation record](../../research/sgx545/hybrid-6.12-build.json)
records the three successful module builds and the ABI checks; hardware testing
is pending.

## Implemented

* An opt-in auxiliary device published by the existing `gma3600` PCI owner.
  The regular display build excludes this code. Even an `SGX_HOST=1` build
  creates the child only with the `sgx_host=1` module parameter.
* A separate identification driver binding that child, with no second PCI
  driver or DRM device. It reads the SGX545 ID/revision registers once and
  exposes a cached sysfs snapshot. It performs no reset, power, interrupt,
  page-table, or display-register writes.
* Patches separating the pinned openpvrsgx services core from its old
  Poulsbo display driver and PCI/DRM registration. The core builds and passes
  modpost using stock Debian headers and symbol exports.
* Replacement of the unexported `__vmalloc_node_range_noprof` dependency:
  cached metadata uses exported `__vmalloc`, and device allocations select
  the existing page-array/vmap path. The patches also fix incorrect vmap flags
  and missing vmap failure handling. Cache aliases and device-memory lifetime
  still require an audit before any runtime activation.
* A fixed-width Intel 1.7 i386 bridge-envelope decoder with bounds/overflow
  tests. The core build checks the actual pinned native DDK envelope is 48 bytes;
  the legacy i386 envelope is 28. This is the outer envelope only, **not a complete
  compat ioctl implementation**. No private ioctls are exposed.

`core/gma3600_sgx_adapter.c` deliberately returns `-EOPNOTSUPP` from module init.
This is a link-check artifact, not a loadable rendering driver. Do not remove
that guard: the legacy PCI acquisition, power/reset, shared IRQ, memory/cache
handling, initialization scripts, and nested bridge payloads are not ported.

## Build the hardware identification checkpoint

On the Atom, in the repository root:

```sh
git pull --ff-only
sudo apt install build-essential linux-headers-$(uname -r) dwarves python3 git patch
bash tests/run.sh
python3 tools/sgx545_hybrid_build.py \
  --kdir /lib/modules/$(uname -r)/build \
  --output /tmp/gma3600-sgx-host1
```

The output directory must be new; choose another name for a repeat build.
There is no installation, library replacement, or module loading in this tool.
It leaves both source checkouts untouched and records commands/results in
`build-report.json`. Check both module files' vermagic against `uname -r`:

```sh
modinfo /tmp/gma3600-sgx-host1/host/gma3600.ko
modinfo /tmp/gma3600-sgx-host1/probe/gma3600_sgx_probe.ko
```

## Test identification on the D2500HN

In GRUB, press `e` and append these temporary arguments to the `linux` line:

```text
module_blacklist=gma500_gfx modprobe.blacklist=gma3600 systemd.unit=multi-user.target
```

Remove `nomodeset` if present and boot with Ctrl-X/F10. These edits last one boot.
From SSH, check `lspci -nnk -s 00:02.0` and `lsmod`: **neither gma500_gfx nor
gma3600 should own the PCI device**. Do not unbind the live graphics driver or
force module removal. If an older driver still owns the device, correct the
temporary boot arguments and reboot first. Keep SSH connected and stream
`sudo dmesg -wH` in another terminal. The
[display test guide](../../docs/D2500HN-VGA-TESTING.md) has logging/recovery details.

The files built in `/tmp` will normally be removed by that reboot. After booting
into the late-loading test session, run the build command above again, then:

```sh
sudo modprobe drm_kms_helper
sudo modprobe drm_ttm_helper
sudo modprobe drm_display_helper
sudo modprobe i2c_algo_bit
sudo insmod /tmp/gma3600-sgx-host1/host/gma3600.ko refclk=27000 sgx_host=1
cat /sys/module/gma3600/version
sudo insmod /tmp/gma3600-sgx-host1/probe/gma3600_sgx_probe.ko
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/identity
lspci -nnk -s 00:02.0
sudo dmesg | grep -Ei 'gma3600|sgx|oops|fault' | tail -100
```

Expected version: `1.0.4-sgx-host1`. PCI ownership remains `gma3600`, the VGA
console remains usable, and the new `identity` file reports raw `core_id` and
`core_revision` values. Please return those outputs and whether VGA stayed
usable. `register_state=unavailable` means zero/all-ones readings: the SGX
island may be powered down, and the probe intentionally does not wake it.
`unverified` means nonzero readings, not validated acceleration or firmware.
The SGX545 offsets are BAR0 + `0x8001c` and `0x80020`; generic gma500 SGX
identification offsets `0x80010/0x80014` must not be substituted.

Unload just the probe with `sudo rmmod gma3600_sgx_probe` when finished. No
permanent boot/module configuration is changed by these steps; rebooting
restores the previously installed display driver. Do not install these test
modules through DKMS yet. `glxinfo` will still report llvmpipe.

## Also reproduce the legacy-core link check (optional)

Use an existing clean checkout at commit
`4000c3fb6577ecad4800e4fa2695777643bfaf86`, or fetch just that commit:

```sh
git init /tmp/openpvrsgx-hybrid
git -C /tmp/openpvrsgx-hybrid remote add origin https://github.com/openpvrsgx-devgroup/linux_openpvrsgx.git
git -C /tmp/openpvrsgx-hybrid fetch --depth=1 --filter=blob:none origin 4000c3fb6577ecad4800e4fa2695777643bfaf86
git -C /tmp/openpvrsgx-hybrid sparse-checkout init --cone
git -C /tmp/openpvrsgx-hybrid sparse-checkout set drivers/gpu/drm/pvrsgx/1.17.4948957/eurasia_km
git -C /tmp/openpvrsgx-hybrid checkout --detach FETCH_HEAD
python3 tools/sgx545_hybrid_build.py \
  --kdir /lib/modules/$(uname -r)/build \
  --source /tmp/openpvrsgx-hybrid \
  --output /tmp/gma3600-sgx-core1
```

`--sgx-rev 10141` is the default **compile profile only**; the tool also accepts
`10131`, which has not been build-validated in this milestone. Neither is a
detected hardware revision. The core output is
`core/gma3600_sgx_core_check.ko`. Build it, but do not install or load it.
The proprietary userspace libraries are not needed for this checkpoint.
Their provenance and static audit are recorded in
[research/sgx545](../../research/sgx545/README.md); they are not bundled here.

## Remaining implementation sequence

1. Use the identification result to select revision/errata and design minimal
   SGX power sequencing. Implement services attachment to the auxiliary child
   with borrowed PCI resources and teardown before parent resources disappear.
2. Port memory/cache ownership and top-level interrupt demultiplexing; bounded
   initialization/recovery must preserve display scanout.
3. Translate the i386 service commands and nested structures explicitly. Begin
   with connect/enumerate/acquire/init, reject unsupported commands, and check
   handle ownership and user-buffer sizes before exposing a private bridge.
4. Run the matching 32-bit initializer and EGL/GLES2 clear/readback, then a
   triangle with GPU completion. Verify pixels and renderer; test malformed
   requests and repeated teardown before enabling broader access.
5. Add presentation and application integration after offscreen rendering works.
   Existing amd64 X/GLX applications still need a separate 64-bit userspace solution.
