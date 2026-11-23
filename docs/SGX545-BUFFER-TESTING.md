# Test shared initializer buffers on Debian 13 amd64

Both modes [passed on the Atom](../research/sgx545/d2500hn-shared-buffers.json).
The next [offline native MMU test](SGX545-MMU-TESTING.md) checks temporary device
addresses and page tables in RAM while GPU execution remains disabled.

Initializer ownership, heap translation, explicit abort and automatic close abort
have [passed on the D2500HN](../research/sgx545/d2500hn-initializer-passed.json).
This checkpoint is services **0.6.0**, with shared CPU-buffer allocation and mmap.
The working `1.0.4-sgx-host2` display parent can stay loaded.

Allocation/free/map-info use explicitly translated Intel 1.7 i386 messages.
Their backing allocator and mapping lifetime are rewritten around the low-32-bit,
zeroed WC page allocator already used by this port. They do not enter the legacy
DDK's global mmap registry or per-process resource manager. The 48-byte client
memory record, 56-byte allocation reply, 56-byte free request and 20-byte map-info
reply are checked against the actual DDK headers compiled in i386 mode.

These are shared system-memory buffers with **no GPU virtual address or DMA
mapping**. GPU reset, firmware, device-heap allocation, submission and SGX IRQ
enablement remain disabled. This is still a restricted diagnostic misc transport;
it cannot yet run the proprietary initializer or accelerate the desktop.

## Build on the Atom

From the repository root, with the pinned source checkout from previous tests:

```sh
git pull --ff-only
bash tests/run.sh
sgx_build="$HOME/Desktop/gma3600-sgx-buffers1"
python3 tools/sgx545_hybrid_build.py \
  --kdir /lib/modules/$(uname -r)/build \
  --source "$HOME/.cache/gma3600/openpvrsgx" \
  --sgx-rev 10131 \
  --output "$sgx_build"
sudo modinfo -F version "$sgx_build/core/gma3600_sgx_services.ko"
"$sgx_build/sgx545-compat-probe" --selftest
```

The output directory must be new; substitute your existing source-cache path if
different. Version must be `0.6.0`. Selftest should include `client_mem=48 alloc=56
map_info=20 mmap2=passed`. No i386 libc or proprietary library installation is
needed. Selftest checks the real i386 mmap2 calling convention using anonymous
RAM; the actual driver mappings are tested below.

## Free and disconnect while mappings remain open

Keep SSH connected and stream `sudo dmesg -wH` in another terminal.
Check the parent and unload any old auxiliary child:

```sh
cat /sys/module/gma3600/version
if [ -d /sys/module/gma3600_sgx_services ]; then sudo rmmod gma3600_sgx_services; fi
if [ -d /sys/module/gma3600_sgx_probe ]; then sudo rmmod gma3600_sgx_probe; fi
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo "$sgx_build/sgx545-compat-probe" --buffers
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
sudo dmesg | grep -Ei 'SGX buffers|SGX bridge|services cleanup|blocked hardware|BUG:|Oops' | tail -20
```

If the parent is absent, use the existing
[late-load procedure](../experimental/sgx545/README.md#test-identification-on-the-d2500hn)
with the host module from your new build. No parent reload is needed when host2
is already running.

The new client repeats the basic bridge tests and then:

* Requires initializer ownership and rejects unsupported flags, invalid sizes,
  wrong file owners, unknown offsets and incorrect mapping lengths.
* Allocates `3 * 4096 + 17` bytes, maps four rounded pages twice, and checks that
  all bytes, including padding, started at zero.
* Writes a pattern through the user mapping and validates it through the kernel
  WC mapping. A separate project diagnostic ioctl then writes another pattern
  through the kernel mapping for readback through both user aliases.
* Splits a mapping with `mprotect`, rejects executable permissions, and checks
  that mappings are excluded from a forked child.
* Frees the buffer handle and disconnects/closes the services FD while mappings
  are live. New accesses through the stale handle fail; existing mappings remain
  readable until their last `munmap` releases the pages.

Expected success line:

```text
PASS: i386 shared buffers, zeroed padding, user/kernel WC roundtrip, map isolation, VMA splits and free/disconnect with live mappings
```

After the client exits, status must include:

```text
files=0 connections=0 bridge_errors=0 heap_handles=0 init_running=0 init_ran=1 buffers=0 buffer_pages=0 buffer_maps=0 buffer_errors=0
```

`pages_live` should return to four and `blocked_hw_accesses` must stay zero.
Unloading should report zero buffers/pages/maps/errors, then the clean bridge
line and `services cleanup: result=0 pages_live=0 blocked_hw_accesses=0`.
The earlier deliberate invalid-device-index test may log
`PVRSRVAcquireDeviceDataKM: requested device is not present` before its PASS.

## Close the FD before freeing its mappings

If the first run and cleanup pass, reload the child for a fresh initializer:

```sh
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1
sudo "$sgx_build/sgx545-compat-probe" --buffers-close
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
sudo dmesg | grep -Ei 'SGX buffers|SGX bridge|services cleanup|blocked hardware|BUG:|Oops' | tail -20
```

This mode closes the FD with owned buffers still mapped. Linux's VMA file
references keep the file/module alive; the final unmap triggers services close,
automatic initializer abort and buffer cleanup. The PASS line ends in
`deferred close cleanup`. The same zero counters and clean unload are required.
Reload between modes because initializer ownership remains one-shot per module
lifetime.

Return both client outputs, status and cleanup lines, and whether the desktop
stayed usable. On failure, capture the logs/status and unload the child after the
client exits. Stop further attempts if cache-restoration errors or retained pages
are reported. Do not force module removal while mappings are open.

## Limits and validation boundary

Only WC allocations are currently accepted: at most 1 MiB each, 16 owned buffers
per file, 64 live buffers and 16 MiB of live pages per provider. Freed-but-mapped
pages stay charged to the global limits. Mappings are shared, non-executable,
excluded from fork/core dumps and cannot expand. A failed WB cache restoration
retains the pages and metadata instead of returning RAM with unsafe aliases.

Host tests cover live auxiliary detach: DDK teardown drops ownership while VMAs
retain independent backing pages, with no late calls into the DDK. Real hardware
detach with open mappings remains unverified. The normal unload tests above must
return all counts to zero. DMA/MMU integration and complete legacy mapping APIs
remain necessary before firmware initialization and a hardware EGL/GLES2 readback.
