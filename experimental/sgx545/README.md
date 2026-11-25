# SGX545 hybrid port for Debian 13 amd64

The hybrid port now has **initializer staging and original Intel enumeration**. It builds
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
including the real compat path and clean teardown.
[Both initializer abort tests passed](../../research/sgx545/d2500hn-initializer-passed.json),
including the disabled shared-PB heap. The
[shared-buffer tests also passed](../../research/sgx545/d2500hn-shared-buffers.json),
including both free and deferred-close lifetimes.
[All ten offline MMU hardware cases passed](../../research/sgx545/d2500hn-offline-mmu.json).
[Both persistent device-memory tests also passed](../../research/sgx545/d2500hn-device-memory.json),
including neighboring mappings, alignment, WC coherence and deferred close.
Version 0.8.1 adds [original Intel payload support and an initializer ABI audit](../../docs/SGX545-INTEL-ABI.md).
The [original payload test and on-target ABI report passed](../../research/sgx545/d2500hn-intel-abi.json).
[Both initializer staging modes passed](../../research/sgx545/d2500hn-initializer-staging.json),
including retained allocations released on unload. Services 0.9.1 fixes the
[original Intel enumeration reply](../../docs/SGX545-ENUMERATION-TESTING.md).
[The 0.9.1 hardware tests passed](../../research/sgx545/d2500hn-enumeration.json).
The next diagnostic is [real Intel initializer capture](../../docs/SGX545-INITIALIZER-CAPTURE.md),
using a private runtime and limited libdrm transport with the existing child.
The [latest hardware run](../../research/sgx545/d2500hn-real-handoff.json) passed
the real initializer's complete 15-buffer/36-page CPU handoff and clean abort.
Services 0.11.0 and transport 0.3.0 measure populated buffer contents before and
after transfer, retaining GPU permissions and reporting hashes/byte counts.
The updated synthetic staging test writes all 11 buffers and detects/restores
CPU changes in staged GPU-read-only code. Rebuild both the child and private
bundle. Legacy command 27 release-mapping support and hardware bring-up remain
unfinished; content checks do not authenticate firmware or prove GPU access.
Hardware execution remains disabled.

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
* Bounded shared CPU buffers with audited Intel 1.7 i386 allocation/free/map-info
  layouts. Their low-32-bit zeroed WC backing and VMA lifetime are rewritten,
  separate from the original DDK mmap registry and resource manager. Opaque
  file-owned handles never expose kernel pointers/PFNs. Mappings survive handle
  release or child detach until the final VMA closes. Tests cover VMA splits,
  owner isolation, quotas, rollback and failed cache restoration. The ELF32
  client's `--buffers` and `--buffers-close` modes check zeroed padding and
  user/kernel WC coherence on hardware. DMA ownership remains pending.
* A bounded native MMU exercise over four borrowed buffer pages and existing
  shared initializer heaps. It reserves VA, maps scatter pages, checks valid PTEs
  and translation readback, and releases all mappings before returning. It also
  tests 4 MiB PT boundary crossings and repeated allocations. The patched native
  free path preserves total heap PTE capacity and bounds clearing by PT index;
  rollback trims empty metadata after partial page-table allocation failures.
  Actual DDK functions run in host fixtures for both profiles. The `--mmu` client
  checks this on the Atom; expired VA results are never usable by a GPU client.

* File-owned persistent allocations in the three existing shared native heaps,
  with Intel 1.7 i386 ALLOC/FREE device-memory translation. Zeroed WC pages stay
  pinned until native PTE removal succeeds; open CPU VMAs have independent
  lifetime. Unsupported sync/cache modes and per-context heaps remain rejected.
  A private readback ioctl verifies PTEs in CPU buffer order without GPU access.
  Host tests cover neighboring allocations, rollback, failed unmap retention and
  close/detach. The `--device` and `--device-close` client modes have both passed on the Atom.

* Project-only initializer staging over the audited 2,068-byte wire model.
  Bounded scripts, size/build checks and file-owned token lookup produce native
  metadata snapshots. An atomic transfer moves referenced mappings to the
  driver, with copyout rollback and same-process control. Creator close does
  not release handed-off pages; explicit abort or child detach removes PTEs
  before backing/common state. Snapshots have no native resource registration
  and are never passed to the hardware initializer. Both staging client modes passed on the Atom. The new `INIT_VALIDATE` ioctl
  runs the same checks without transferring ownership, consuming tokens or
  ending the initializer attempt. The updated `--init-stage` mode adds repeat
  validation and validation-copyout-fault checks; these additions passed on the Atom. Legacy command 99 stays blocked in the kernel.

`core/gma3600_sgx_adapter.c` now registers a preparation-only auxiliary driver
when explicitly loaded with `prepare=1`. It starts the OS/memory services and
DDK initialization part 1, then enumerates the SGX device. It borrows the parent's
mapping, never acquires/enables PCI again, and omits the original procfs, mmap
registry and unrestricted dispatcher. The diagnostic bridge requires an additional
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

1. Test explicit abort and unload cleanup of staged initializer buffers. The
   original Intel payload forms and on-target layout audit have passed, along
   with preparation, bridge, initializer, shared-buffer, offline MMU and both
   persistent-allocation tests.
2. Extend explicit nested-payload translation to remaining initializer memory
   and mapping commands, retaining ownership and rollback.
   Add the necessary DRM transport after validating the diagnostic subset.
3. Port device DMA ownership, match the initializer/microkernel build, and
   connect the legacy ISR through the parent's interrupt API. Design bounded
   power/reset/recovery before allowing GPU execution.
4. Run the matching 32-bit initializer and EGL/GLES2 clear/readback, then a
   triangle with GPU completion. Verify pixels and renderer; test malformed
   requests and repeated teardown before enabling broader access.
5. Add presentation and application integration after offscreen rendering works.
   Existing amd64 X/GLX applications still need a separate 64-bit userspace solution.
