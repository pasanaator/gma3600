# Verified Intel SGX545 legacy artifacts

The selected route is a hybrid SGX port for Debian 13 amd64. The normal gma3600
kernel module remains version **1.0.4**. These research tools do not enable
acceleration. The [first implementation and hardware test](../../experimental/sgx545/README.md)
now separate the services core from its old display driver and produce a
6.12.117 link-check module. The unmodified-build failure below is retained as
the baseline, not the result of the patched hybrid build.

The user subsequently supplied the
[successful D2500HN identification checkpoint](d2500hn-sgx-identification.json):
raw ID `01150000`, revision `0001000d` (1.0.13), with the same VGA behavior.
The newer [IRQ implementation/build record](hybrid-irq-6.12-build.json)
covers the parent-owned interrupt API and an additional `10131` compile profile;
hardware interrupt delivery and firmware/profile compatibility remain untested.

The next [services preparation checkpoint](../../docs/SGX545-SERVICES-TESTING.md)
adds real CPU-side DDK initialization, borrowed resources, modern cache handling,
partial-init cleanup, and noncontiguous reset-page addresses. Its build/test
evidence is in [services-prepare-6.12-build.json](services-prepare-6.12-build.json).
No proprietary libraries are needed or executed in this checkpoint.

The user has now [passed services preparation and cleanup](d2500hn-services-preparation.json)
on the Atom: one device, four tracked pages, zero after cleanup, and no blocked
register accesses. The [i386 diagnostic bridge test](../../docs/SGX545-BRIDGE-TESTING.md)
adds a restricted root-only misc transport and an ELF32 client. Its connect,
enumerate, acquire and disconnect commands operate on real CPU-side DDK objects;
firmware, rendering and a complete legacy DRM interface remain unfinished.
The [bridge build/test record](restricted-bridge-6.12-build.json) separates host
and emulated-client checks from the subsequent real kernel compat test.

That [real kernel compat test has now passed on the Atom](d2500hn-i386-bridge.json),
including copyout rollback, fork/file isolation and zero-page cleanup. The next
[initializer checkpoint](../../docs/SGX545-INITIALIZER-TESTING.md) adds exclusive
ownership, real read-only DDK heap metadata and abort handling. It retains the
GPU-execution block and is still a diagnostic transport rather than a complete
legacy DRM services interface.
See the [initializer build/test record](initializer-info-6.12-build.json) for the
two compile profiles, actual i386 header assertions and mocked lifetime checks.
The [first initializer hardware attempt](d2500hn-initializer-attempt.json) retrieved
metadata but stopped at an overly strict client check for a disabled zero-size
PB heap. Cleanup remained clean. The corrected client accepts that DDK record;
the [test guide](../../docs/SGX545-INITIALIZER-TESTING.md#retry-after-fail-invalid-heap-range-result9)
has a client-only rebuild/retest procedure with the existing 0.5.0 module.

The corrected [explicit and automatic initializer tests have now passed](d2500hn-initializer-passed.json)
with all handles/pages released. The next
[shared-buffer checkpoint](../../docs/SGX545-BUFFER-TESTING.md) adds bounded
allocation/free/map-info commands and CPU mmap with independent VMA lifetimes.
The backing allocator and mmap provider are rewritten rather than exposing the
old DDK resource manager or mmap registry. Hardware DMA/rendering remains disabled.
The [shared-buffer build/test record](shared-buffers-6.12-build.json) records both
compile profiles, the i386 layouts and mocked VMA/cache-lifetime verification.

Both [shared-buffer hardware modes passed](d2500hn-shared-buffers.json), including
the real i386 WC mapping roundtrip and zero-page cleanup. The next
[offline MMU checkpoint](../../docs/SGX545-MMU-TESTING.md) reuses native VA and
scatter/PTE/free routines on RAM. It fixes total-PTE-capacity bookkeeping and
checks partial PT rollback; it does not enable hardware MMU access or rendering.
The [offline MMU build/test record](offline-mmu-6.12-build.json) lists the native
functions exercised, failure cases and both successful kernel compile profiles.

[The offline MMU test passed on the Atom](d2500hn-offline-mmu.json): ten cases,
all PTE readbacks correct, no remaining page tables, and zero-page cleanup.
Services 0.8.0 now adds [persistent device memory](../../docs/SGX545-DEVICE-MEMORY-TESTING.md),
with Intel 1.7 allocation/free wire translation, backing-page pins, and native
PTE lifetime independent of open CPU VMAs. The
[build and host-test record](device-memory-6.12-build.json) covers both profiles
and the actual i386 public-header layouts.
[Both device-memory modes passed on the Atom](d2500hn-device-memory.json), with
all three heaps, clean neighboring mappings and zero-page teardown.

The [original Intel kernel-source ABI audit](initializer-abi-6.12-audit.json)
uses an independently checksum-verified Ubuntu `cedarview-drm` source package
from the same 20120717 release. It found CONNECT and device-free payload-size
differences from the community compatibility headers. Services 0.8.1 supports
both forms. The initializer model is 2,068 bytes on i386 versus 2,192 bytes with
actual amd64 kernel handles. Both compile profiles passed the new audit;
[the original-payload hardware test and on-target ABI report passed](d2500hn-intel-abi.json).
Services 0.9.0 adds [initializer staging](../../docs/SGX545-STAGING-TESTING.md): native
metadata translation and driver-owned buffer lifetime, with no GPU execution.
[Both build profiles and host lifetime/translation tests passed](staging-6.12-build.json);
the two staging hardware modes are pending.

The [mapping-release hardware checkpoint](d2500hn-mapping-release.json) passed
all 15 Intel releases, with no live CPU VMAs at staging, unchanged populated
buffers, valid native PTEs and clean teardown. Services 0.13.0 / transport 0.5.0
add a bounded actual native `InitDevInfo`/`DeinitDevInfo` CPU cycle and a CCB slot
write/readback/restore; see [the build record](native-cycle-6.12-build.json).
GPU firmware execution, hardware DMA, native resource registration and rendering
remain unverified.

The [transient native CPU cycle passed on the Atom](d2500hn-native-cycle.json),
including the real Intel initializer, 15 unchanged buffers and zero-page unload.
Services 0.14.0 / transport 0.6.0 now add retained file-owned binding and native
query/unbind, abort/close/detach ordering and cache-bit preservation. See the
[retained runtime build record](native-runtime-6.12-build.json).
This is still CPU-only native integration; it does not register BM objects,
run firmware, enable IRQ/power or establish GPU execution.

The [retained native binding checkpoint passed](d2500hn-native-runtime.json),
including both synthetic lifetime modes and the real Intel initializer.
The next [native BM build checkpoint](native-bm-6.12-build.json) tests real native
wrapping/freeing through temporary kernel-only aliases of pinned pages. It also
extends the independent Intel compiler audit to the shared TA/transfer layouts.
See the [hardware instructions](../../docs/SGX545-INITIALIZER-CAPTURE.md).

The [native BM hardware test passed](d2500hn-native-bm.json), including both
alias cycles on all 15 real Intel buffers and clean native/OS/page teardown.
The next [retained native allocator checkpoint](native-allocator-6.12-build.json)
adds `native_bm=1` to keep native BM and OS objects from allocation through
staging and cleanup. Modern backing-page and CPU VMA ownership remain separate;
legacy ResMan/free and actual GPU execution are still unfinished.

## Retrieved artifact and provenance

The Ubuntu archive provides `cedarview-graphics-drivers`, version
`20120717-0ubuntu1`, architecture `i386`. Its `.deb` is 3,187,994 bytes with SHA256:

```text
819254e7d78d078b90d6b22c6ec19f6fba470ad13f1e9a0eae366d51cbcdf46b
```

[cedarview-20120717.json](cedarview-20120717.json) records URLs, sizes, hashes,
and the verification chain. The Precise-updates Release detached signature
verified with GnuPG's default policy using Ubuntu's archived signing key,
fingerprint `630239CC130E1A7FD81A27B140976EAF437D05B5`, obtained from the official
Ubuntu keyring over HTTPS. Its signature date is 2021-05-04 and its digest is
SHA1. No verification bypass flags were used. SHA256/size checks then verified
both the package index and the `.deb`; the signed source index also verified
the matching upstream archive containing public API headers. This establishes
archival provenance, not modern security support or runtime compatibility.

No binary files are included in this repository, installed, or executed. The
package contains Intel/Imagination binary license terms, including restrictions
on modification and reverse engineering; preserve and review its copyright
file when obtaining copies. Inspection here uses package control fields, ELF
headers, dynamic dependencies, public exports, and symbol-version metadata.
There is no disassembly, `ldd`, or execution of candidate libraries.

## Reproduce the metadata audit

On a development machine with Python 3, `dpkg-deb`, and `readelf`:

```sh
curl -fL --proto '=https' \
  https://old-releases.ubuntu.com/ubuntu/pool/multiverse/c/cedarview-graphics-drivers/cedarview-graphics-drivers_20120717-0ubuntu1_i386.deb \
  -o /tmp/cedarview-graphics-drivers_20120717-0ubuntu1_i386.deb
python3 tools/sgx545_legacy_audit.py \
  /tmp/cedarview-graphics-drivers_20120717-0ubuntu1_i386.deb \
  --output /tmp/cedarview-audit.json
```

The audit refuses a size/hash mismatch **before** extraction or inspection.
It extracts the pinned package to a temporary directory without running any
maintainer scripts, reads metadata with `readelf`, emits JSON, and removes its
extracted copy. It does not install the package or alter the dynamic linker.
The downloader is not an installer. Do not run `dpkg -i` on the historical driver
as a shortcut to acceleration on Debian 13.

[cedarview-20120717-audit.json](cedarview-20120717-audit.json) is the actual report:
21 ELF files, all ELF32/i386, with these useful components:

| Component under `/usr/lib/intel-cdv` | Static evidence |
| --- | --- |
| `libEGL.so.1.7.862890` | Exports EGL context and pbuffer APIs, including `eglCreatePbufferSurface`. |
| `libGLESv2.so.1.7.862890` | Exports GLES2 clear, draw, finish, and readback entry points. |
| `libsrv_um.so.1.7.862890` | Exports services connection, device enumeration, and memory operations. |
| `libsrv_init.so.1.7.862890` | Exports `SrvInit`; an initializer component is present, but its execution/build-option compatibility is untested. |
| `libusc.so` / `libglslcompiler.so` versioned files | Compiler components are present; no proprietary compiler was executed. |
| `libpvrPVR2D_*WSEGL.so` versioned files | Several window-system backends exist; selection/runtime requirements need investigation. |
| `xorg/modules/drivers/pvr_drv.so` | Old Xorg driver; imports `libudev.so.0` and `libwsbm.so.1`. |

The package also contains `/usr/lib/i386-linux-gnu/dri/pvr_dri.so`. The package
control dependencies include `xorg-video-abi-11`, `cedarview-drm`, `libudev0`, and
`libwsbm1`. This is not a drop-in Debian 13 X driver.

Following only `DT_NEEDED` from EGL and GLES2 finds these external sonames:
`libc.so.6`, `libdl.so.2`, `libdrm.so.2`, `libm.so.6`, `libpthread.so.0`, and
`librt.so.1`. That smaller static closure makes an isolated offscreen test worth
pursuing, but it excludes dynamically selected initializer/compiler/WSEGL modules.
The API exports and old GLIBC requirements do not prove EGL initialization or
hardware rendering. No 32-bit libraries have been installed on the user's Atom.

## Actual kernel build probe

The community candidate is pinned to:

```text
https://github.com/openpvrsgx-devgroup/linux_openpvrsgx
4000c3fb6577ecad4800e4fa2695777643bfaf86
```

The 1.17.4948957 core has an Intel 1.7.862890 ABI compatibility configuration.
A direct build against Debian `6.12.117+deb13-amd64` headers was attempted in an
isolated directory. The initial attempt lacked a parameter-buffer allocation
selection; the corrected probe selects `CONFIG_SGX_PB_PER_CONTEXT=y`, consistent
with the inspected Intel 1.7 build file. The candidate's SGX revision 10141 is
used only to reproduce its compile target, not as a detected hardware revision.

The corrected build **fails** in `dc_poulsbo/poulsbo_linux.c` and
`poulsbo_linux_sdvo.c`, including:

* Removed `drm_device.pdev` and `drm_fb_helper.fbdev` fields.
* Changed fb-helper prepare/init/initial-config signatures and a removed
  connector-registration helper.
* Framebuffer creation and connector mode-validation callback type mismatches.

See [openpvrsgx-6.12-build-probe.json](openpvrsgx-6.12-build-probe.json) for the
configuration and exact compiler messages. No `.ko` was produced or loaded.
Successfully compiled individual core objects do not establish a working driver.

To reproduce, use a sparse checkout rather than downloading unrelated kernel
files, then export/build the clean tracked candidate with the helper:

```sh
git clone --filter=blob:none --no-checkout --depth 1 --single-branch \
  --branch linux+pvrsgx https://github.com/openpvrsgx-devgroup/linux_openpvrsgx.git \
  /tmp/openpvrsgx-research
git -C /tmp/openpvrsgx-research sparse-checkout init --cone
git -C /tmp/openpvrsgx-research sparse-checkout set \
  drivers/gpu/drm/pvrsgx/1.17.4948957/eurasia_km
git -C /tmp/openpvrsgx-research fetch --depth 1 origin \
  4000c3fb6577ecad4800e4fa2695777643bfaf86
git -C /tmp/openpvrsgx-research checkout --detach \
  4000c3fb6577ecad4800e4fa2695777643bfaf86
bash tools/sgx545_kernel_build_probe.sh \
  --source /tmp/openpvrsgx-research \
  --kdir /lib/modules/$(uname -r)/build \
  --output /tmp/sgx545-build-probe
```

Use a new output directory each time. The helper preserves the underlying make
failure status. It never installs or loads the result. Different headers may
produce different errors. This is a build experiment, not instructions to
replace the working gma3600 module.

## Next implementation boundary

The archived package availability blocker is resolved. The next substantive
work is to separate reusable SGX services from the old Poulsbo display owner,
audit the complete i386-to-amd64 bridge payload layouts, and integrate PCI/MMIO,
IRQ, memory, and power ownership with the existing gma3600 driver. Porting the
legacy display callbacks merely to compile them would not solve that ownership
problem. gma3600 and a second PCI driver cannot both independently bind the
same GMA3600 function.

After a real services/compat bridge and matching initialization path exist, the
first runtime acceptance target is a 32-bit offscreen EGL/GLES2 clear/readback
with hardware completion and no software fallback. A compiler-clean module,
EGL symbol exports, or a renderer string alone will not be accepted as success.
