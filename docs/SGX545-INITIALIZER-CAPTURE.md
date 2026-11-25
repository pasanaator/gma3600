# Capture the real Intel initializer's requests

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
init_build="$HOME/Desktop/gma3600-sgx-initializer-$(date +%Y%m%d-%H%M%S)"
python3 tools/sgx545_initializer_build.py --output "$init_build"
python3 tools/sgx545_initializer_run.py --bundle "$init_build" --selftest
```

The final line must be `PASS: private i386 loader, project-owned fixture and
limited libdrm transport`. This selftest executes only the open-source fixture,
including in cloud validation under QEMU. It does not initialize the GPU.
The pinned dependency list and validation results are recorded in
[the build record](../research/sgx545/initializer-capture-build.json).

## Capture on the Atom, using a fresh services child

Keep the working host2 display parent loaded and SSH connected. Reuse
`$sgx_build` from the successful 0.9.1 test; it points to that existing build.
The kernel/display modules do not need rebuilding for this tool.

```sh
modinfo -F version "$sgx_build/core/gma3600_sgx_services.ko"
if [ -d /sys/module/gma3600_sgx_services ]; then
  sudo rmmod gma3600_sgx_services
fi
if [ -d /sys/module/gma3600_sgx_probe ]; then
  sudo rmmod gma3600_sgx_probe
fi
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1
sudo python3 tools/sgx545_initializer_run.py \
  --bundle "$init_build" --capture \
  --report "$init_build/initializer-report.json"
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
sudo dmesg | grep -Ei 'SGX buffers|SGX bridge|services cleanup|blocked hardware|BUG:|Oops' | tail -10
```

The helper verifies the Intel PCI vendor/device, `gma3600` parent ownership,
host2/services 0.9.1 versions, fresh initialization state and zero live resources.
It refuses capture through QEMU or on a different GPU. It checks the bundle
hashes before execution and uses only the private runtime/library path. The
capture has a 25-second timeout and saves the initializer output and before/after
resource counts in JSON. A nonzero initializer return with clean resources is
a completed capture, not successful GPU initialization. A crash, timeout,
unexpected success or leftover resources is marked `inspect_report`.

After process exit, expect `pages_live=4`, `gpu_execution=disabled`, and zero
file, allocation, mapping, stage, error and blocked-access counters. `init_ran`
may be zero or one depending on how far the real initializer got. Unload must
report `result=0 pages_live=0 blocked_hw_accesses=0`.

Return `initializer-report.json`, the printed output and cleanup lines. Each
new capture requires a fresh services load and a new report filename.
The desktop should continue using llvmpipe. Native BM/resource integration,
firmware matching, active DMA/page walks and power/reset/IRQ bring-up remain
unfinished.
