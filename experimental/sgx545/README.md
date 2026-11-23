# SGX545 hybrid port for Debian 13 amd64

The hybrid port now has an **initializer ownership and heap-info checkpoint**. It builds
against Debian `6.12.117+deb13-amd64`. Hardware 3D is **not enabled yet**.
The first rendering target is an i386 EGL/GLES2 offscreen process on the amd64
kernel. The available Intel libraries do not accelerate amd64 applications.
The [build validation record](../../research/sgx545/hybrid-6.12-build.json)
records the first three successful module builds and ABI checks. The user has
now [confirmed the identification checkpoint](../../research/sgx545/d2500hn-sgx-identification.json)
on the Atom with the same display behavior as before. Rendering remains untested.
The [CPU-side services test](../../docs/SGX545-SERVICES-TESTING.md) has now passed:
one device, four tracked pages, zero pages after cleanup and no blocked register
accesses. The [i386 bridge also passed on the Atom](../../research/sgx545/d2500hn-i386-bridge.json),
including the real compat path and clean teardown. The next
[hardware test exercises initializer ownership and heap metadata](../../docs/SGX545-INITIALIZER-TESTING.md)
without enabling GPU execution. It uses the already loaded host2 driver.

## D2500HN hardware result

The supplied SSH output reports `core_id=0x01150000` and
`core_revision=0x0001000d`, whose revision bytes decode to **1.0.13**. PCI
ownership remains `gma3600`, and the log shows the working 1920x1080 VGA mode.
The user reports that everything loaded as before. No probe-related oops or
fault appears in the supplied excerpt. This establishes readable SGX registers
and display coexistence for this boot, without power/reset writes. It does not
establish firmware compatibility, working SGX interrupts, or hardware rendering.

Do not convert `1.0.13` directly into the build token `10131` or `10141`.
The original Intel 1.7 Makefile selects `10131`, and both tokens are accepted by
the pinned newer SGX545 errata header. The microkernel's revision/build-option
checks must still agree with the chosen profile. The `10131` services core has
now also been build-validated; that is not a hardware compatibility result.

## Implemented

* An opt-in auxiliary device published by the existing `gma3600` PCI owner.
  The regular display build excludes this code. Even an `SGX_HOST=1` build
  creates the child only with the `sgx_host=1` module parameter.
* A separate identification driver binding that child, with no second PCI
  driver or DRM device. It reads the SGX545 ID/revision registers once and
  exposes a cached sysfs snapshot. It performs no reset, power, interrupt,
  page-table, or display-register writes.
* A parent-owned SGX interrupt routing API. Registration leaves the SGX source
  masked; an explicit enable operation preserves the vblank bits in IER/IMR.
  The child ISR must clear SGX-local events before the parent acknowledges IIR.
  Unregister masks the source and waits for active IRQ callbacks before their
  data can be freed. Unhandled SGX interrupts are masked to prevent a storm.
  Host tests exercise simultaneous SGX/vblank delivery and concurrent detach.
  The identification probe does not use or enable this API, and the legacy
  services ISR is not attached yet. The new host interface is ABI 2, module
  `1.0.4-sgx-host2`; build its companion probe from the same checkout.
* Patches separating the pinned openpvrsgx services core from its old
  Poulsbo display driver and PCI/DRM registration. The core builds and passes
  modpost using stock Debian headers and symbol exports.
* Replacement of the unexported `__vmalloc_node_range_noprof` dependency:
  cached metadata uses exported `__vmalloc`, and device allocations select
  the existing page-array/vmap path. The patches also fix incorrect vmap flags
  and missing vmap failure handling. The preparation allocator now manages low
  zeroed pages and WB/WC/UC direct-map cache attributes, restores WB before
  freeing RAM, and retains allocations if that restoration fails. The amd64
  protection macros use Linux's PAT helpers instead of silently mapping WC
  requests uncached. Device DMA, CMA and userspace buffer paths remain unvalidated.
* Correct reset-buffer physical addresses for noncontiguous RAM, and cleanup
  of partially initialized SGX contexts/heaps. Mocked tests compile the actual
  patched DDK functions to check reset addresses and blocked register accesses.
* Explicit Intel 1.7 i386 layouts for the outer envelope and restricted
  connect/enumerate/acquire/disconnect payloads. The native envelope is 48 bytes
  versus 28 on the wire; CONNECT's output is 16 native bytes versus 12 on i386.
  With `prepare=1 bridge=1`, a root-only diagnostic misc device translates these
  commands to real DDK enumeration/device and secure-handle functions. Handles
  are file-owned opaque IDs, never native pointers. Tests cover malformed
  payloads, unsupported commands, and allocation rollback after copyout faults.
  A freestanding ELF32 client tests the kernel compat path on the Atom. This is
  **not a complete legacy bridge** and is not exposed through DRM yet.
* Initializer CONNECT with the real DDK privilege/state checks, exclusive
  file ownership, abort-only DISCONNECT and automatic abort on close/detach.
  SGX initializer INFO calls the DDK's read-only metadata function and explicitly
  translates the page-directory address and 32 heap records. Shared heaps receive
  file-owned opaque handles; repeated queries reuse them. Tests cover partial
  allocation, cache/rollback failures and teardown. Actual DDK public headers
  compile in i386 mode to assert the 776-byte reply and each heap field offset.
  Successful finalization remains rejected, and a completed attempt is one-shot
  per module load. The client adds `--init-info` and `--init-close` tests.

`core/gma3600_sgx_adapter.c` now registers a preparation-only auxiliary driver
when explicitly loaded with `prepare=1`. It starts the OS/memory services and
DDK initialization part 1, then enumerates the SGX device. It borrows the parent's
mapping, never acquires/enables PCI again, and omits legacy procfs, mmap and the
original unrestricted dispatcher. The diagnostic bridge requires an additional
`bridge=1` parameter. Legacy register reads/writes are blocked and counted; part 2 and
system finalization reject execution. No firmware or SGX interrupt is enabled.
Without `prepare=1`, module init still returns `-EOPNOTSUPP`.

## Build the hardware identification checkpoint

On the Atom, in the repository root:

```sh
git pull --ff-only
sudo apt install build-essential linux-headers-$(uname -r) dwarves python3 git patch
bash tests/run.sh
python3 tools/sgx545_hybrid_build.py \
  --kdir /lib/modules/$(uname -r)/build \
  --output /tmp/gma3600-sgx-host2
```

The output directory must be new; choose another name for a repeat build.
There is no installation, library replacement, or module loading in this tool.
It leaves both source checkouts untouched and records commands/results in
`build-report.json`. Check both module files' vermagic against `uname -r`:

```sh
modinfo /tmp/gma3600-sgx-host2/host/gma3600.ko
modinfo /tmp/gma3600-sgx-host2/probe/gma3600_sgx_probe.ko
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
sudo insmod /tmp/gma3600-sgx-host2/host/gma3600.ko refclk=27000 sgx_host=1
cat /sys/module/gma3600/version
sudo insmod /tmp/gma3600-sgx-host2/probe/gma3600_sgx_probe.ko
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/identity
lspci -nnk -s 00:02.0
sudo dmesg | grep -Ei 'gma3600|sgx|oops|fault' | tail -100
```

Expected version: `1.0.4-sgx-host2`. PCI ownership remains `gma3600`, the VGA
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

## Build the services preparation module

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
`10131`, which has also passed the services-core build. Neither token has been
matched to a running microkernel on this board. The current core output is
`core/gma3600_sgx_services.ko`. Follow the
[preparation test](../../docs/SGX545-SERVICES-TESTING.md) for its explicit
`prepare=1` load, status checks, and unload verification. Earlier build records
name the historical `gma3600_sgx_core_check.ko`, which did not support preparation.
The proprietary userspace libraries are not needed for this checkpoint.
Their provenance and static audit are recorded in
[research/sgx545](../../research/sgx545/README.md); they are not bundled here.

## Remaining implementation sequence

1. Validate initializer ownership, heap metadata and explicit/automatic abort on
   the Atom. CPU-side preparation and the restricted i386 bridge have passed.
2. Extend explicit nested-payload translation to initializer memory-allocation
   and mapping commands, retaining ownership, rollback and request validation.
   Add the necessary DRM transport after validating the diagnostic subset.
3. Port device DMA ownership, match the initializer/microkernel build, and
   connect the legacy ISR through the parent's interrupt API. Design bounded
   power/reset/recovery before allowing GPU execution.
4. Run the matching 32-bit initializer and EGL/GLES2 clear/readback, then a
   triangle with GPU completion. Verify pixels and renderer; test malformed
   requests and repeated teardown before enabling broader access.
5. Add presentation and application integration after offscreen rendering works.
   Existing amd64 X/GLX applications still need a separate 64-bit userspace solution.
