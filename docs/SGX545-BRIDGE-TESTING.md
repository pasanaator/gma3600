# Test the i386 diagnostic bridge on Debian 13 amd64

The previous services preparation test passed on the D2500HN: one SGX device,
four tracked pages, zero blocked accesses, and zero pages after unloading.
The [recorded hardware result](../research/sgx545/d2500hn-services-preparation.json)
is for that checkpoint, before this bridge existed.

This checkpoint adds `/dev/gma3600-sgx-bridge`, root-only by default. A real
ELF32/i386 program exercises CONNECT, ENUMERATE, ACQUIRE and DISCONNECT against
the real DDK's CPU-side device and secure-handle functions on the amd64 kernel.
It checks the two ioctl aliases, malformed messages, copyout rollback, stale and
cross-file handles, fork isolation, and automatic close cleanup. All nested
payloads use explicit Intel 1.7 i386 layouts, including the 12-byte CONNECT reply.

This is a diagnostic misc device, not a DRM rendering node or a complete legacy
binary interface. No proprietary libraries are used. GPU reset, firmware,
submission, mmap and SGX interrupt enablement remain disabled; llvmpipe is still
expected. The normal display driver has no changes in this checkpoint.

## Build

From the repository root on the Atom, with the pinned source checkout retained
from the [services test](SGX545-SERVICES-TESTING.md#build-on-the-atom):

```sh
git pull --ff-only
bash tests/run.sh
python3 tools/sgx545_hybrid_build.py \
  --kdir /lib/modules/$(uname -r)/build \
  --source "$HOME/.cache/gma3600/openpvrsgx" \
  --sgx-rev 10131 \
  --output /tmp/gma3600-sgx-bridge1
/tmp/gma3600-sgx-bridge1/sgx545-compat-probe --selftest
modinfo /tmp/gma3600-sgx-bridge1/core/gma3600_sgx_services.ko
```

Use your existing clean pinned checkout path if different. The output directory
must be new. The tool also builds the freestanding i386 client with `-m32`;
no multilib libc, i386 packages or proprietary graphics libraries are needed.
The selftest must report `ELF32 i386 executable`, envelope 28, CONNECT 12,
ENUMERATE 200 and handle 8. It tests execution/layout, not the driver.
`Exec format error` here means the running kernel cannot execute this i386
binary; check `CONFIG_IA32_EMULATION` before continuing.

The services module version should be `0.4.0`, with vermagic matching `uname -r`.
The SGX compile token remains unverified against running firmware.

## Load only the child

Keep SSH connected and stream `sudo dmesg -wH` in another terminal. The parent
must already be `1.0.4-sgx-host2` with its auxiliary child present:

```sh
cat /sys/module/gma3600/version
ls /sys/bus/auxiliary/devices/gma3600.sgx.*
```

If necessary, follow the existing
[parent late-load instructions](../experimental/sgx545/README.md#test-identification-on-the-d2500hn).
In particular, `insmod` does not load dependencies; `sudo modprobe i2c_algo_bit`
supplies `i2c_bit_add_bus`. Do not unload the working parent to test this child.
Unload an existing services or identification child first, if present:

```sh
if [ -d /sys/module/gma3600_sgx_services ]; then sudo rmmod gma3600_sgx_services; fi
if [ -d /sys/module/gma3600_sgx_probe ]; then sudo rmmod gma3600_sgx_probe; fi
sudo insmod /tmp/gma3600-sgx-bridge1/core/gma3600_sgx_services.ko prepare=1 bridge=1
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
ls -l /dev/gma3600-sgx-bridge
```

Expected status:

```text
state=prepared devices=1 pages_live=4 blocked_hw_accesses=0 gpu_execution=disabled bridge=enabled files=0 connections=0 bridge_errors=0
```

`insmod` success alone does not prove the auxiliary probe succeeded. If the status
file or device node is absent, return the kernel logs before retrying.
Loading with `prepare=1` alone retains the previous preparation-only behavior.

## Run and check cleanup

```sh
sudo /tmp/gma3600-sgx-bridge1/sgx545-compat-probe
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
sudo dmesg | grep -Ei 'gma3600|services|sgx|BUG:|Oops' | tail -100
```

Expected client output includes both `PASS: real DDK device index=... type=SGX`
and `PASS: i386 compat bridge, handles, validation, copyout rollback, fork isolation and close cleanup`.
After the client exits, status must show `files=0 connections=0 bridge_errors=0`
and `blocked_hw_accesses=0`. Unloading should report:

```text
gma3600 SGX bridge stopped: files=0 connections=0 errors=0
services cleanup: result=0 pages_live=0 blocked_hw_accesses=0
```

Return the client output, status, cleanup lines and whether the desktop remained
usable. On a failed test, capture status/logs and unload the child; do not retry
if cleanup reports retained allocations or blocked accesses. If it passes, repeat
the child load/client/status/unload once to test a fresh module lifetime.

Open files hold the services module in use. Auxiliary unbind first removes the
misc node, serializes with active requests, and drains file-owned DDK handles
before common services teardown; surviving file descriptors become offline.
Unexpected handle cleanup failures are counted and retain their CPU allocations
rather than permitting later access through freed owners.

A passing result establishes the restricted i386-to-amd64 bridge and its handle
lifetime. It does not establish firmware initialization or rendering. Next are
the initializer-specific nested payloads, DMA ownership, bounded reset/power and
parent IRQ integration, followed by an isolated i386 EGL/GLES2 readback test.
