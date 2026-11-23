# SGX545 hardware acceleration: legacy compatibility investigation

Status: the selected **hybrid port for Debian 13 amd64** has its first implementation.
The separated legacy services core builds against stock 6.12.117 headers, and an
opt-in auxiliary child provides SGX545 identification without a second PCI owner.
The user confirmed raw ID `01150000` and revision `0001000d` (1.0.13) while
preserving the prior display behavior. The implementation includes parent-owned
interrupt routing, verified with host tests but not hardware SGX interrupt delivery.
Hardware rendering is not enabled. See the
[implementation and Atom test instructions](../experimental/sgx545/README.md).
The [CPU-side services checkpoint](SGX545-SERVICES-TESTING.md) passed on the Atom,
including zero tracked pages after cleanup and zero blocked register accesses.
The next [i386 bridge test](SGX545-BRIDGE-TESTING.md) uses explicit nested layouts
for four commands, real DDK secure handles, and a freestanding 32-bit client.
The Intel i386 userspace artifact has been archive-verified and statically audited;
see [the artifact record](../research/sgx545/README.md). The normal display build
remains version 1.0.4, with SGX host support excluded by default.

## What is established

The D2500HN's GMA3600 combines Intel display hardware with a PowerVR SGX545
(Series 5XT) rendering core. Our code implements KMS and GEM dumb framebuffers.
It has no SGX command submission, SGX address-space management, GPU job fences,
GPU reset/recovery, shader compiler, or matching accelerated userspace library.
The user confirmed that X and the desktop render correctly using llvmpipe;
that is CPU OpenGL rendering and is expected for the current implementation.

The upstream Linux PowerVR driver is for Series 6 and later. Its Linux 6.12
Kconfig also depends on ARM64. Mesa's PowerVR Vulkan driver starts with Rogue;
its documentation explicitly excludes pre-Series6 GPUs. Enabling those drivers,
adding DRIVER_RENDER, or creating a render node cannot make SGX545 accelerate.
A legacy SGX route should target a supported OpenGL/GLES workload first, not
promise Vulkan support on this generation.

## A concrete legacy lead

The community openpvrsgx tree contains:

* An Intel DDK 1.7.862890 option described as originating from Ubuntu 12.04 and
  intended for Cedarview.
* A Cedarview SGX545 target in its build file.
* A 1.17.4948957 build option for the 1.7.862890 ABI, plus an ioctl alias option
  defaulting to 18 for Cedarview.
* Cedarview platform integration sources, including PCI and display dependencies.

These are source-level leads, not evidence of a working Debian 13 configuration.
The inspected 1.17 Cedarview sysconfig contains an old `drm/drmP.h` include,
but it is guarded for kernels before 5.5 and is not itself the 6.12 blocker.
An actual 6.12.117 build, with the per-context parameter-buffer option selected,
reaches failures in the bundled Poulsbo display/DRM fb-helper callbacks.
The platform glue also retains disabled legacy DRM integration code. The original
1.7 build file names SGX core revision 10131; the top-level Cedarview target names
10141. Do not select a revision solely from either default: actual silicon,
firmware, ABI options, and any applicable errata must agree.

The inspected Maemo userspace package supplies TI343x/TI443x libraries and
explicitly declares `Architecture: armhf`. Those packages cannot supply native
x86 userspace libraries for an Atom. The Intel package `cedarview-graphics-drivers_20120717-0ubuntu1_i386.deb` has now
been retrieved from Ubuntu's historical archive and verified against its signed
Precise-updates package index. It contains ELF32/i386 DDK 1.7.862890 libraries.
This confirms a candidate artifact, not kernel/runtime compatibility.

No external driver was installed, and no proprietary library was executed or
copied into this repository. The unmodified candidate fails to build; the
experimental patches now produce a services link-check module. That module
deliberately rejects initialization, and runtime compatibility remains unproven.

## Collect the Atom's compatibility inventory

Pull this repository and run as your ordinary desktop user:

```sh
python3 tools/sgx545_inventory.py --glxinfo > sgx545-inventory.json
```

Over SSH, omit `--glxinfo` unless you have deliberately connected to the actual
X session. The existing llvmpipe result is already known. The tool reads PCI
metadata, resource ranges, DRM-node bindings, module version, Debian architecture,
selected compatibility configuration if readable, and ELF library headers.
It does not load a module, install packages, read GPU MMIO, reset the GPU, or
run a candidate proprietary library. On this cloud host it completed with no
Cedarview device found; cloud execution does not validate the Atom's hardware.

If you already possess a candidate Intel userspace library, inspect its header:

```sh
python3 tools/sgx545_inventory.py --library /path/to/candidate/libEGL.so
```

The report does not contain the binary. A matching ELF architecture is necessary
but does not establish the DDK ABI, firmware, GPU revision, or kernel compatibility.
Review the report before sharing it; it includes system/library version metadata.

## D2500HN target inventory result

The user supplied the inventory from the running Debian installation on
2026-11-22. The relevant facts are:

| Item | Observed result | Consequence |
| --- | --- | --- |
| Device | Intel `8086:0be1`, revision `09`, bound to gma3600 1.0.4 at inventory time | A later auxiliary probe read SGX ID `01150000`, revision `0001000d` (1.0.13), with display coexistence confirmed. DDK profile/microkernel matching is still pending. |
| Kernel and userspace | `6.12.117+deb13-amd64`, amd64 userspace | Native applications require 64-bit libraries. |
| Kernel compat options | `CONFIG_IA32_EMULATION=y`, `CONFIG_COMPAT=y` | A 32-bit process can be investigated without replacing the kernel, provided its loader/libraries and a correct driver compat ABI exist. |
| Foreign Debian architectures | None enabled | There is no configured i386 multiarch environment yet; this does not prove every possible 32-bit binary is absent. |
| Graphics libraries in scanned locations | ELF64 x86-64 GL/GLX/EGL/GLES libraries; no verified legacy SGX candidates | Matching Intel DDK userspace was not installed at inventory time; a candidate archive has since been verified separately. The scan is not an exhaustive filesystem search. |
| Renderer | Mesa 25.0.7, llvmpipe (LLVM 19.1.7) | `direct rendering: Yes` still describes software rendering in this report. |
| DRM nodes for this PCI device | `card0` only | Consistent with display-only support; creating a render node is not the missing acceleration implementation. |
| PCI resources | BAR 0 is 1 MiB; BAR 2 and BAR 3 have zero-sized assignments | Preserve the working display resource handling; do not infer usable SGX mappings from nonexistent BARs. |

The initial legacy target should therefore be a **32-bit offscreen test process
on the existing amd64 kernel**, contingent on obtaining compatible Intel i386
libraries and implementing/validating the required kernel bridge. This is a
research target, not proof that this combination will work. It would not
accelerate the existing 64-bit desktop by installing 32-bit libraries alone.
Do not prescribe adding i386 packages until the actual library and dependency
requirements are known.

The additional bridge audit at the same pinned openpvrsgx commit found that
`PVRSRV_BRIDGE_PACKAGE` embeds `IMG_HANDLE` fields. In `img_types.h`, kernel
handles are pointers, while Linux userspace handles can be 32-bit or selected
64-bit values. `pvr_compat_ioctl` passes private commands onward to `drm_ioctl`;
that function alone does not translate these private payloads. Audit the full
bridge and build options for size, alignment, nested pointers, and handle
translation before making any amd64/i386 compatibility claim. The DDK-version
ABI option is not by itself proof of Linux 32-on-64 ioctl compatibility.

The archive network prerequisite is resolved. The historical package has
been verified and inspected without installation. Its old Xorg binary depends
on video ABI 11 and obsolete system libraries, so it cannot replace the working
Debian 13 graphics stack. EGL/GLES2 offscreen experimentation remains a candidate,
subject to the services bridge, initializer, firmware/build-option compatibility,
and dynamically loaded window-system components.

## The next implementation gates

1. **Identify a matching userspace stack.** Record the Intel package version,
   provenance, redistribution terms, ELF class/machine, runtime dependencies,
   DDK ABI and build options, GPU revision, and any firmware or initialization
   payloads. Identify the actual GL/EGL/GLES entry points it supports. Do not
   assume that a desktop GLX library, an EGL library, and GLES are interchangeable.
   A 32-bit renderer may be tested on a suitable 64-bit kernel with an explicit
   compat ABI; it does not accelerate existing 64-bit desktop processes.
2. **Reproduce a reference configuration.** Use a disposable disk/system image
   for a known legacy stack if needed; preserve the working Debian installation.
   An offscreen GLES triangle or clear/readback is the first rendering target.
   Record actual renderer, pixel result, GPU completion, and failure behavior.
3. **Port the kernel services with explicit ownership.** Audit the legacy bridge
   ABI, handle lifetimes, fixed-width/compat layouts, MMU mappings, interrupts,
   synchronization, firmware initialization, power, reset, and timeout handling.
   gma3600 owns PCI 0000:00:02.0: a second PCI driver cannot independently bind
   that device. Reuse code through a component owned by the existing driver or
   a deliberately designed split; do not load competing legacy display modules.
   SGX MMU mappings are distinct from the display GTT, and SGX reset/power must
   not corrupt scanout. Scope early submission access to controlled testing
   until its validation and isolation are established.
4. **Prove offscreen work on Debian 13.** Exercise GPU writes, completion/fences,
   readback, invalid handles/ranges, timeout/reset, close with jobs in flight,
   and repeated context destruction. A renderer name or successful module load
   is insufficient. Never silently substitute llvmpipe in a hardware test.
5. **Integrate presentation.** Design buffer ownership, layout, cache handling,
   and fencing between renderer and KMS. Implement PRIME/dma-buf interoperation
   if the selected userspace architecture needs it; the current dumb-buffer
   path does not supply that integration. Then test EGL/GLES presentation,
   X integration where available, and boot/suspend/regression cases.

Gate 1 has an authenticated historical Intel userspace artifact and a static
library/dependency audit. Gate 3 has started: services/display build separation,
an opt-in parent-owned auxiliary interface, a hardware identification probe, and
an outer i386 bridge-envelope decoder. The kernel build checks the pinned DDK's
48-byte native envelope against the separate 28-byte i386 wire layout. CPU-side
services activation and cleanup have now passed on the Atom; see the
[hardware result](../research/sgx545/d2500hn-services-preparation.json).
Connect/enumerate/acquire/disconnect translation is implemented for the next
diagnostic test, including CONNECT's differently aligned 64-bit handle field.
Initializer payloads, device DMA, power/IRQ integration and rendering remain
unfinished. No DRM rendering interface is exposed yet.

## Inspected source references

All compatibility statements above refer to inspected files, not untested
project descriptions being treated as hardware validation.

* [Linux v6.12 PowerVR Kconfig](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/imagination/Kconfig): Series 6+ scope and ARM64 dependency.
* [Mesa PowerVR documentation, mirror commit 22c08cc53bd024a1a9f971c005a6fd862648eca6](https://github.com/chaotic-cx/mesa-mirror/blob/22c08cc53bd024a1a9f971c005a6fd862648eca6/docs/drivers/powervr.rst): Rogue scope and explicit pre-Series6 exclusion. This is a mirror; the canonical project is [Mesa](https://gitlab.freedesktop.org/mesa/mesa).
* [openpvrsgx Kconfig, commit 4000c3fb6577ecad4800e4fa2695777643bfaf86](https://github.com/openpvrsgx-devgroup/linux_openpvrsgx/blob/4000c3fb6577ecad4800e4fa2695777643bfaf86/drivers/gpu/drm/pvrsgx/Kconfig): Intel DDK and compat ABI options.
* [openpvrsgx build targets at the same commit](https://github.com/openpvrsgx-devgroup/linux_openpvrsgx/blob/4000c3fb6577ecad4800e4fa2695777643bfaf86/drivers/gpu/drm/pvrsgx/Makefile): SGX545/Cedarview target and revision 10141.
* [Intel 1.7 build file at the same commit](https://github.com/openpvrsgx-devgroup/linux_openpvrsgx/blob/4000c3fb6577ecad4800e4fa2695777643bfaf86/drivers/gpu/drm/pvrsgx/1.7.862890/pvr/Makefile): x86 Cedarview flags and revision 10131.
* [1.17 Cedarview sysconfig at the same commit](https://github.com/openpvrsgx-devgroup/linux_openpvrsgx/blob/4000c3fb6577ecad4800e4fa2695777643bfaf86/drivers/gpu/drm/pvrsgx/1.17.4948957/eurasia_km/services4/system/cedarview_linux/sysconfig.c): legacy PCI/DRM integration needing review and porting.
* [1.17 bridge declarations](https://github.com/openpvrsgx-devgroup/linux_openpvrsgx/blob/4000c3fb6577ecad4800e4fa2695777643bfaf86/drivers/gpu/drm/pvrsgx/1.17.4948957/eurasia_km/services4/include/pvr_bridge.h), [handle types](https://github.com/openpvrsgx-devgroup/linux_openpvrsgx/blob/4000c3fb6577ecad4800e4fa2695777643bfaf86/drivers/gpu/drm/pvrsgx/1.17.4948957/eurasia_km/include4/img_types.h), and [DRM compat dispatch](https://github.com/openpvrsgx-devgroup/linux_openpvrsgx/blob/4000c3fb6577ecad4800e4fa2695777643bfaf86/drivers/gpu/drm/pvrsgx/1.17.4948957/eurasia_km/services4/srvkm/env/linux/pvr_drm.c): private ABI layouts need a complete 32/64-bit audit.
* [Maemo sgx-ddk-um control, commit 6fb7fa24d8aa3d7d97b835b42e464e95bbdf0588](https://github.com/maemo-leste/sgx-ddk-um/blob/6fb7fa24d8aa3d7d97b835b42e464e95bbdf0588/debian/control): ARM-only TI userspace package targets.

Test the inventory parser with:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_sgx545_inventory.py' -v
```

`bash tests/run.sh` also runs these tests alongside the existing driver host tests.
