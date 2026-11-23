# Test initializer ownership and heap metadata on the Atom

The [restricted i386 bridge passed](../research/sgx545/d2500hn-i386-bridge.json)
on the D2500HN: the real 32-bit client completed its tests and all file, handle,
error and tracked-page counts returned to zero after cleanup.

Services version **0.5.0** adds Intel 1.7 initializer CONNECT, abort-only DISCONNECT,
and SGX initializer INFO. The last command calls the real DDK's read-only
`SGXGetInfoForSrvinitKM`, translates its kernel page-directory address and 32 heap
records into a **776-byte i386 reply**, and assigns file-owned opaque IDs to shared
heaps. Native pointers/handles are never copied into that reply.

The initializer requires the DDK's `CAP_SYS_MODULE` privilege check and has one
exclusive owning file. A completed or abandoned attempt is one-shot per module
load. Successful finalization remains rejected; no firmware, reset, submission,
SGX interrupt enablement or device DMA test occurs. The existing KMS parent and
llvmpipe desktop remain the display stack. This remains a diagnostic misc
transport, not the complete DRM interface required by the proprietary libraries.

## Build

In the repository root, using the pinned source checkout from the previous test:

```sh
git pull --ff-only
bash tests/run.sh
sgx_build="$HOME/Desktop/gma3600-sgx-init1"
python3 tools/sgx545_hybrid_build.py \
  --kdir /lib/modules/$(uname -r)/build \
  --source "$HOME/.cache/gma3600/openpvrsgx" \
  --sgx-rev 10131 \
  --output "$sgx_build"
modinfo -F version "$sgx_build/core/gma3600_sgx_services.ko"
"$sgx_build/sgx545-compat-probe" --selftest
```

Substitute your actual pinned checkout path if different. The output directory
must be new. The module version must be `0.5.0`; selftest now also reports
`heap=24 init_info=776`. The builder compiles the real pinned public DDK headers
in i386 mode to verify structure sizes, field offsets and complete command
encodings against our wire definitions. No i386 libc or proprietary library is
needed for these compile-only checks or the freestanding client.

## Explicit abort test

Keep SSH connected and `sudo dmesg -wH` running in another terminal. Reuse the
loaded `1.0.4-sgx-host2` parent; do not unload it. If it is absent, use the existing
[parent late-load procedure](../experimental/sgx545/README.md#test-identification-on-the-d2500hn)
with the host build in your selected output directory.

```sh
cat /sys/module/gma3600/version
if [ -d /sys/module/gma3600_sgx_services ]; then sudo rmmod gma3600_sgx_services; fi
if [ -d /sys/module/gma3600_sgx_probe ]; then sudo rmmod gma3600_sgx_probe; fi
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo "$sgx_build/sgx545-compat-probe" --init-info
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
sudo dmesg | grep -Ei 'SGX bridge|services|blocked hardware|BUG:|Oops' | tail -40
```

Initial status must have `state=prepared`, `bridge=enabled`, `init_running=0`,
`init_ran=0`, `heap_handles=0`, and no errors/blocked accesses.
The client repeats the previous bridge tests, then checks initializer ownership,
failed-copy rollback, device-token validation, stable repeated heap queries,
rejection of successful finalization, explicit abort and rejection of another
initializer attempt. It prints the actual page-directory address and heap records.

The new success line is:

```text
PASS: initializer ownership, 776-byte heap reply, repeat query, rollback and explicit abort
```

After the client exits, status must have:

```text
files=0 connections=0 bridge_errors=0 heap_handles=0 init_running=0 init_ran=1
```

`pages_live=4` and `blocked_hw_accesses=0` should remain unchanged. `init_ran=1`
means the CPU-side attempt ended; it does not mean GPU initialization succeeded.
The final bridge log must have zero files, connections, errors, heap handles and
running initializers. Services cleanup must again report
`result=0 pages_live=0 blocked_hw_accesses=0`.

## Automatic abort on close

If the explicit test and cleanup pass, reload the child for a fresh attempt:

```sh
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1
sudo "$sgx_build/sgx545-compat-probe" --init-close
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
sudo dmesg | grep -Ei 'SGX bridge|services|blocked hardware|BUG:|Oops' | tail -40
```

This mode closes the owner while heap handles are live, without an explicit
initializer abort. Its success line ends in `automatic close abort`; the same
zero-handle and cleanup counts must follow. Reload the child between initializer
tests: the native DDK's one-shot state deliberately rejects another attempt in
the same module lifetime. The plain client invocation still runs the earlier
bridge suite without claiming initializer ownership.

Return both client outputs (including heap records), status and cleanup lines,
and whether the desktop stayed usable. If a test fails, capture status/logs and
unload the child. Stop further attempts if cleanup retains allocations or reports
blocked accesses. Matching initializer memory-allocation/mapping operations,
device DMA ownership, firmware compatibility and bounded power/reset/IRQ handling
remain prerequisites for the first EGL/GLES2 hardware readback.
