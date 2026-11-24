# Test persistent SGX device allocations on the Atom

**Hardware result:** both 0.8.0 modes passed on all three shared heaps, with zero
remaining allocations, mappings, errors and tracked pages after unload. See
[the D2500HN result](../research/sgx545/d2500hn-device-memory.json).
The next test checks [original Intel payload forms](SGX545-INTEL-ABI.md).

The [0.7.0 offline MMU test passed on the D2500HN](../research/sgx545/d2500hn-offline-mmu.json),
including all ten scatter/boundary/reuse cases and zero-page cleanup. Services
**0.8.0** adds file-owned device allocations that remain mapped between bridge
calls. The three shared initializer heaps use native DDK VA reservation,
scatter mapping, PTE lookup and free routines, with rewritten backing-page and
ownership handling.

The Intel 1.7 i386 allocation/free payloads are translated explicitly: requests
are 24/12 bytes and replies are 76/4 bytes in the compatibility forms tested
with 0.8.0. Version 0.8.1 also accepts the original Intel 60-byte free request
and 8-byte reply. This is a restricted implementation
of those commands on the project diagnostic device, not a complete legacy
userspace interface. Only read/write WC allocations with `NO_SYNCOBJ` are
accepted. There is no sync object, fence, per-context heap, DMA submission or
firmware execution. **GPU execution remains disabled; llvmpipe is expected.**

Each allocation is limited to 1 MiB, with page-sized through 1 MiB power-of-two
alignment (zero selects 4 KiB). The existing shared/device backing pool enforces
16 owned buffers per file in 0.8.x (32 in 0.9.0), 64 live buffers and 16 MiB total. CPU mappings cover
the rounded, zeroed backing, while `alloc_size` reports the requested size.
Device addresses remain reserved until free/disconnect/close. File-owned tokens
never expose kernel pointers. CPU mapping handles use the existing map-info and
mmap2 path.

A separate backing-page reference remains held until native unmapping succeeds.
Free/disconnect/close removes PTEs before dropping that reference. Existing CPU
VMAs retain the backing independently and may outlive the device allocation.
If native unmapping verification fails, the driver retains the RAM and ownership
records, reports `device_errors`, and rejects further allocations on that file.
A CPU-only shared-buffer free cannot remove a device allocation's backing.

## Build

From the repository root, using the same pinned source cache as before:

```sh
git pull --ff-only
bash tests/run.sh
sgx_build="$HOME/Desktop/gma3600-sgx-device1"
python3 tools/sgx545_hybrid_build.py \
  --kdir /lib/modules/$(uname -r)/build \
  --source "$HOME/.cache/gma3600/openpvrsgx" \
  --sgx-rev 10131 \
  --output "$sgx_build"
modinfo -F version "$sgx_build/core/gma3600_sgx_services.ko"
"$sgx_build/sgx545-compat-probe" --selftest
```

Choose a new output directory if that one already exists. Expected module
version: `0.8.1` in current builds (`0.8.0` for the recorded hardware run). Selftest includes `device_alloc=76 device_check=24`. Both compile
profiles have host tests; this guide continues the `10131` compile profile.
Neither profile has yet been matched to running SGX firmware.

## Explicit disconnect test

Keep SSH connected and `sudo dmesg -wH` streaming in another terminal. Reuse the
loaded `1.0.4-sgx-host2` parent. If it is not loaded, use the existing
[late-load instructions](../experimental/sgx545/README.md#test-identification-on-the-d2500hn)
and the host module from this build directory.

```sh
cat /sys/module/gma3600/version
if [ -d /sys/module/gma3600_sgx_services ]; then sudo rmmod gma3600_sgx_services; fi
if [ -d /sys/module/gma3600_sgx_probe ]; then sudo rmmod gma3600_sgx_probe; fi
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1
sudo "$sgx_build/sgx545-compat-probe" --device
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
sudo dmesg | grep -Ei 'SGX buffers|SGX bridge|services cleanup|blocked hardware|BUG:|Oops' | tail -10
```

The client allocates two buffers in each of the three shared heaps, including
64 KiB alignment. It checks zeroed padding, WC CPU/user/kernel coherence, and
native PTE physical addresses in CPU buffer order. It frees one buffer with its
CPU VMA still open, verifies the neighbor remains mapped, and allocates/frees a
replacement. It also checks malformed requests, cross-file and stale tokens,
wrong-kind free, and rollback after failed allocation copyout. One live device
allocation and the CPU VMAs remain for final disconnect/close cleanup.

Expected output includes three `DEVICE heap=... sibling_free=preserved
CPU_mapping=preserved` lines and:

```text
PASS: i386 persistent device allocations, aligned VA, CPU-order PTEs, WC roundtrip, sibling lifetime, rollback and disconnect with live mappings
```

After the client exits, expect `pages_live=4`, `blocked_hw_accesses=0`,
`init_running=0`, `init_ran=1`, `device_allocations=0`, `device_pages=0`,
`device_errors=0`, and all file/connection/heap/buffer/map/error counters zero.
After unload, services cleanup must report `result=0 pages_live=0
blocked_hw_accesses=0`.

## Deferred close test: fresh services load

Each initializer attempt is one-shot. Reload the child before this second mode:

```sh
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1
sudo "$sgx_build/sgx545-compat-probe" --device-close
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
sudo dmesg | grep -Ei 'SGX buffers|SGX bridge|services cleanup|blocked hardware|BUG:|Oops' | tail -10
```

This ends with `deferred close cleanup` instead of explicit disconnect. Linux
VMAs retain the file, so final file release and native cleanup may occur at the
last unmap. Expect the same zero counters afterward. Neither the test nor VMA
close enables GPU execution or accesses registers through the DDK.

Return both client outputs, final status and cleanup lines, and whether the
desktop remained usable. The invalid-device-index message from the ordinary
bridge checks is deliberate. If a test fails, retain the status/logs and unload
the services child after the client exits. Do not continue after retained pages,
`device_errors`, an oops, or blocked hardware accesses.

The next prerequisites are remaining initializer memory commands, full legacy
transport compatibility, DMA ownership, matching firmware, and bounded
power/reset/interrupt handling before an offscreen GLES test can execute.
