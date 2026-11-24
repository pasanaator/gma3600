# Original Intel ABI audit and test

Both services 0.8.0 device-memory modes [passed on the D2500HN](../research/sgx545/d2500hn-device-memory.json).
All three heaps preserved neighboring allocations and CPU mappings, and both
unloads released every tracked page with no blocked hardware access.

Services **0.8.1** fixes payload differences found by comparing the original
Intel kernel headers with the newer community compatibility headers. Existing
diagnostic payloads remain supported. This does not enable GPU execution.

| Payload | Original Intel 20120717 i386 | Community 1.7 compatibility i386 |
| --- | ---: | ---: |
| CONNECT reply | 8 bytes | 12 bytes |
| Device-free request | 60 bytes | 12 bytes |
| Device-free reply | 8 bytes | 4 bytes |
| Initializer part-2 request | 2,068 bytes | 2,068 bytes |

The old device-free request includes a 48-byte client-memory record after its
three header words. The bridge copies the bounded payload but trusts only its
owned device and memory tokens. It never follows those trailing client pointers.
The original reply's extra word is zeroed. Tests cover exact output lengths,
malformed sizes, stale tokens and failed-copy cleanup for both forms.

The matching amd64 **kernel** request is **2,192 bytes**, with a 2,176-byte
initializer body versus 2,060 bytes for i386. Public userspace headers compiled
in 64-bit mode are not a substitute for measuring the kernel's handle types.
The new build embeds a small non-executable layout table produced with the
actual Kbuild flags. The audit reads that object's ELF section without loading
the module.

The fixed-width initializer model is checked against both sets of actual
headers. Command 99 remains rejected. Implementing it still requires explicit
field/handle translation, native memory wrappers and an ownership handoff so
initializer buffers can outlive the creating process. Script validation,
firmware/build matching, DMA ownership and bounded power/reset/IRQ handling
remain necessary before GPU execution.

## Hardware test of the corrected payload forms

Reuse the loaded `1.0.4-sgx-host2` parent. From the repository root:

```sh
git pull --ff-only
bash tests/run.sh
sgx_build="$HOME/Desktop/gma3600-sgx-intelabi1"
python3 tools/sgx545_hybrid_build.py \
  --kdir /lib/modules/$(uname -r)/build \
  --source "$HOME/.cache/gma3600/openpvrsgx" \
  --sgx-rev 10131 \
  --output "$sgx_build"
modinfo -F version "$sgx_build/core/gma3600_sgx_services.ko"
"$sgx_build/sgx545-compat-probe" --selftest
if [ -d /sys/module/gma3600_sgx_services ]; then sudo rmmod gma3600_sgx_services; fi
if [ -d /sys/module/gma3600_sgx_probe ]; then sudo rmmod gma3600_sgx_probe; fi
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1
sudo "$sgx_build/sgx545-compat-probe" --device-intel
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
sudo dmesg | grep -Ei 'SGX buffers|SGX bridge|services cleanup|blocked hardware|BUG:|Oops' | tail -10
```

Use a new output directory. Expect version `0.8.1`. Keep SSH connected and kernel
logs streaming as before. If the parent is absent, follow the existing
[late-load instructions](../experimental/sgx545/README.md#test-identification-on-the-d2500hn).

This repeats the proven allocation test using 8-byte CONNECT replies and
60-byte/8-byte device-free payloads, with an extra final line:

```text
PASS: original Intel CONNECT=8 FREE_DEVICE_IN=60 FREE_DEVICE_OUT=8 payload forms
```

Expect `pages_live=4` before unload, zero allocation/mapping/handle/error counters,
`init_ran=1`, and `blocked_hw_accesses=0`. Unload must report `result=0
pages_live=0 blocked_hw_accesses=0`. Return that output and whether the desktop
stayed usable. GPU execution stays disabled and llvmpipe remains expected.
The original 0.8.0 `--device` and `--device-close` tests need not be repeated for
this checkpoint.

## Reproduce the compiler audit without GPU hardware

The reference is the original Ubuntu `cedarview-drm` DKMS **source** package,
version `20120717-0ubuntu1`, i386. Its archive metadata and checksum are recorded
with [the audit results](../research/sgx545/initializer-abi-6.12-audit.json).
The new tool verifies the exact package size and SHA256 before extraction; it
never installs it or runs package scripts. It compiles layout constants and
reads ELF sections, and does not execute any resulting object or proprietary
library. `cc`, `objcopy`, and `dpkg-deb` are required; 32-bit libc is not needed.

After downloading the package from the recorded Ubuntu archive URL, run:

```sh
python3 tools/sgx545_init_abi_audit.py \
  --intel-package /path/to/cedarview-drm_20120717-0ubuntu1_i386.deb \
  --compat-core "$sgx_build/core" \
  --output /tmp/gma3600-init-abi-audit1
```

The output directory must be new. The tool checks original and compatibility
public headers in real `-m32` mode, runs the command/field-offset assertions
against each, and reads the actual amd64 kernel layout table. It records known
payload forms separately from unsupported differences. Selected firmware
control sizes/offsets and build-option checks are only part of compatibility;
they do not prove that the proprietary userspace or firmware will run.
