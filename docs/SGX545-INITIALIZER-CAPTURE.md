# Capture the real Intel initializer's requests

**Current services: 0.9.4; transport: 0.1.4.** The real Intel initializer
[passes indexed ACQUIRE and obtains heap information](../research/sgx545/d2500hn-initializer-allocation.json).
Its first allocation is 8 KiB aligned to 4 KiB, with flags **0x4f**: read/write,
cache-consistent, no sync object, and EDM-protected. Services 0.9.3 accepted only
the earlier diagnostic flags and rejected this request. Abort and unload still
released every tracked resource without blocked hardware accesses.

Services 0.9.4 accepts the observed coherent/EDM flags, optional zeroing and
RAM-backed flags, and an omitted CPU cache mode. Intel's original BM allocator
inherits heap attributes in that case. The private provider still supports
only validated WC system-memory heaps: direct-map, kernel and user aliases all
stay WC. Other CPU cache modes remain unsupported. Replies now include inherited
heap attributes, and per-allocation/staging metadata retains the actual flags.

The native MMU mapper now writes requested CACHECONSISTENT and EDMPROTECT bits.
Its verifier checks those and access bits as well as physical page order. Source
tests cover both bits separately/together, deliberate PTE corruption, sibling
mappings, cleanup and mixed staged allocation flags. This verifies CPU-visible
PTE construction; active GPU DMA coherence and protection remain untested.

Reserved bridge words remain ignored, and the two documented PDump-only CONNECT
flags remain no-ops. Unknown service bits are rejected and PDUMP builds are
explicitly disabled. These fields do not grant privileges or extend lifetimes.

Rebuild both the services module and private initializer bundle for this
checkpoint. The display parent remains the working host2 build.

The [0.9.1 hardware tests passed](../research/sgx545/d2500hn-enumeration.json).
The next diagnostic invokes the **unmodified Intel `SrvInit()`** with the
original `libsrv_init.so` and `libsrv_um.so`, on the user's Intel GMA3600.
This replaces synthetic client requests with the initializer's actual requests.

A private, project-owned `libdrm.so.2` opens only `/dev/gma3600-sgx-bridge` and
translates the documented Intel libdrm transport to the existing diagnostic
ioctl. It logs command numbers, payload lengths, allocation flags/sizes/alignment,
kernel errors and the initializer's return value. It does not dump firmware
contents or change the proprietary libraries. Unknown commands are rejected;
command 99 and successful initializer finalization are always blocked. This is
a limited compatibility transport, not a full libdrm implementation or security
sandbox. Its master-related calls do not acquire or release a KMS master.

**GPU execution remains disabled.** A successful `SrvInit()` is not expected.
The useful result is the first actual compatibility gap and whether all
resources are released when the process exits. The real initializer may stop
before allocation, at unsupported flags, or at another missing service. The
result must guide the next change; its stage is not assumed in advance.

## Build privately, without installing old packages

The builder downloads the checksum-pinned Intel package and Ubuntu i386
`libc6`/`libc6-dev` 2.15 packages into a cache. It extracts them into a new output
directory and builds the adapter, launcher and a project-owned fixture. It
does not install packages, change system library paths, load modules or execute
the proprietary libraries. Their license is retained as `INTEL-LICENSE.txt`.
No 32-bit system libc or multilib CRT installation is needed. The existing
compiler's `-m32` support, Linux UAPI headers, Python 3 and `dpkg-deb` are used.

From the repository root on the Atom:

```sh
git pull --ff-only
bash tests/run.sh
sgx_build="$HOME/Desktop/gma3600-sgx-memory-$(date +%Y%m%d-%H%M%S)"
python3 tools/sgx545_hybrid_build.py \
  --kdir /lib/modules/$(uname -r)/build \
  --source "$HOME/.cache/gma3600/openpvrsgx" \
  --sgx-rev 10131 --output "$sgx_build"
modinfo -F version "$sgx_build/core/gma3600_sgx_services.ko"
init_build="$HOME/Desktop/gma3600-sgx-initializer-$(date +%Y%m%d-%H%M%S)"
python3 tools/sgx545_initializer_build.py --output "$init_build"
python3 tools/sgx545_initializer_run.py --bundle "$init_build" --selftest
```

The final line must be `PASS: private i386 loader, project-owned fixture and
limited libdrm transport`, with `transport=0.1.4` in the loader line. The module
must report version `0.9.4`. This selftest
checks the observed PCI-only selector and rejects other devices without opening
hardware. It executes only the open-source fixture,
including in cloud validation under QEMU. It does not initialize the GPU.
The pinned dependency list and validation results are recorded in
[the build record](../research/sgx545/initializer-capture-build.json).
The [current validation record](../research/sgx545/initializer-memory-6.12-build.json)
covers both kernel profiles, original flag definitions, real native MMU source,
cache/EDM PTE mutations, metadata preservation and lifetime tests.

## Capture on the Atom, using a fresh services child

Keep the working host2 display parent loaded and SSH connected. Reuse
`$sgx_build` from the new 0.9.4 build above. Load only the new services child;
the working display parent stays loaded.

```sh
modinfo -F version "$sgx_build/core/gma3600_sgx_services.ko"
if [ -d /sys/module/gma3600_sgx_services ]; then
  sudo rmmod gma3600_sgx_services
fi
if [ -d /sys/module/gma3600_sgx_probe ]; then
  sudo rmmod gma3600_sgx_probe
fi
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1
sudo "$sgx_build/sgx545-compat-probe" --device-intel
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
```

Stop if the allocation test fails. Expect this additional line:

```text
PASS: Intel flags=0x4f, inherited WC cache mode, coherent/EDM PTE checks and CPU roundtrip
```

The test checks all three initializer heaps, neighboring allocations, CPU
mapping lifetime and kernel PTE verification. Before unload expect four baseline
pages and zero allocations/maps/errors; unload must release all pages.
**Reload services before capture:** the allocation test consumes the one-shot
initializer attempt.

```sh
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1
sudo python3 tools/sgx545_initializer_run.py \
  --bundle "$init_build" --capture \
  --report "$init_build/initializer-report.json"
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
sudo dmesg | grep -Ei 'SGX buffers|SGX bridge|services cleanup|blocked hardware|BUG:|Oops' | tail -10
```

The helper verifies the Intel PCI vendor/device, `gma3600` parent ownership,
host2/services 0.9.4 versions, fresh initialization state and zero live resources.
It refuses capture through QEMU or on a different GPU. It checks the bundle
hashes before execution and uses only the private runtime/library path. The
capture has a 25-second timeout and saves the initializer output and before/after
resource counts in JSON. A nonzero initializer return with clean resources is
a completed capture, not successful GPU initialization. A crash, timeout,
unexpected success or leftover resources is marked `inspect_report`.
The trace includes `CONNECT reserved=... service_flags=...` and
`ACQUIRE reserved=... index=... type=...`. Return those values even if a request
is still rejected. Unsupported selectors, flags and commands are not silently
rewritten by the userspace transport.
Successful allocations also log the returned handle, device address, size and
effective flags, including inherited CPU cache attributes.

After process exit, expect `pages_live=4`, `gpu_execution=disabled`, and zero
file, allocation, mapping, stage, error and blocked-access counters. `init_ran`
may be zero or one depending on how far the real initializer got. Unload must
report `result=0 pages_live=0 blocked_hw_accesses=0`.

Return `initializer-report.json`, the printed output and cleanup lines. Each
new capture requires a fresh services load and a new report filename.
The desktop should continue using llvmpipe. Native BM/resource integration,
firmware matching, active DMA/page walks and power/reset/IRQ bring-up remain
unfinished.
