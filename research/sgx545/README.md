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
