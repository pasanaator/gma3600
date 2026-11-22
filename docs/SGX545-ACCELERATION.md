# SGX545 hardware acceleration: legacy compatibility investigation

Status: investigation and read-only inventory tooling, not an accelerated driver.
The user selected the legacy compatibility route on 2026-11-22. Keep the working
1.0.4 display kernel module while investigating a separate rendering stack.

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
The inspected 1.17 Cedarview sysconfig still includes `drm/drmP.h` (absent from
our 6.12.117 headers) and has disabled legacy DRM integration code. The original
1.7 build file names SGX core revision 10131; the top-level Cedarview target names
10141. Do not select a revision solely from either default: actual silicon,
firmware, ABI options, and any applicable errata must agree.

The inspected Maemo userspace package supplies TI343x/TI443x libraries and
explicitly declares `Architecture: armhf`. Those packages cannot supply native
x86 userspace libraries for an Atom. A matching Intel x86 DDK package has not
been retrieved or verified. Attempts at the current Ubuntu pool locations
returned 404, while the old-releases request returned 403 in this environment;
that is not evidence the historical package no longer exists.

No external driver was installed, no proprietary library was executed or copied
into this repository, and no claim is made that the inspected SGX tree builds
or works on Linux 6.12.117.

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

Until gate 1 produces usable x86 artifacts and their ABI requirements, kernel
ioctls or firmware interfaces would be guesses. The current milestone supplies
the inventory tool and this pinned compatibility record, not hardware 3D.
If suitable legacy artifacts cannot be obtained, the alternatives are a new
open SGX userspace stack/compiler or retaining software rendering.

## Inspected source references

All compatibility statements above refer to inspected files, not untested
project descriptions being treated as hardware validation.

* [Linux v6.12 PowerVR Kconfig](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/imagination/Kconfig): Series 6+ scope and ARM64 dependency.
* [Mesa PowerVR documentation, mirror commit 22c08cc53bd024a1a9f971c005a6fd862648eca6](https://github.com/chaotic-cx/mesa-mirror/blob/22c08cc53bd024a1a9f971c005a6fd862648eca6/docs/drivers/powervr.rst): Rogue scope and explicit pre-Series6 exclusion. This is a mirror; the canonical project is [Mesa](https://gitlab.freedesktop.org/mesa/mesa).
* [openpvrsgx Kconfig, commit 4000c3fb6577ecad4800e4fa2695777643bfaf86](https://github.com/openpvrsgx-devgroup/linux_openpvrsgx/blob/4000c3fb6577ecad4800e4fa2695777643bfaf86/drivers/gpu/drm/pvrsgx/Kconfig): Intel DDK and compat ABI options.
* [openpvrsgx build targets at the same commit](https://github.com/openpvrsgx-devgroup/linux_openpvrsgx/blob/4000c3fb6577ecad4800e4fa2695777643bfaf86/drivers/gpu/drm/pvrsgx/Makefile): SGX545/Cedarview target and revision 10141.
* [Intel 1.7 build file at the same commit](https://github.com/openpvrsgx-devgroup/linux_openpvrsgx/blob/4000c3fb6577ecad4800e4fa2695777643bfaf86/drivers/gpu/drm/pvrsgx/1.7.862890/pvr/Makefile): x86 Cedarview flags and revision 10131.
* [1.17 Cedarview sysconfig at the same commit](https://github.com/openpvrsgx-devgroup/linux_openpvrsgx/blob/4000c3fb6577ecad4800e4fa2695777643bfaf86/drivers/gpu/drm/pvrsgx/1.17.4948957/eurasia_km/services4/system/cedarview_linux/sysconfig.c): legacy PCI/DRM integration needing review and porting.
* [Maemo sgx-ddk-um control, commit 6fb7fa24d8aa3d7d97b835b42e464e95bbdf0588](https://github.com/maemo-leste/sgx-ddk-um/blob/6fb7fa24d8aa3d7d97b835b42e464e95bbdf0588/debian/control): ARM-only TI userspace package targets.

Test the inventory parser with:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_sgx545_inventory.py' -v
```

`bash tests/run.sh` also runs these tests alongside the existing driver host tests.
