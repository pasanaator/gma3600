# SGX545 services preparation on Debian 13 amd64

This checkpoint initializes the real DDK's CPU-side services, registers its SGX
device, and allocates the kernel MMU context and reset buffers. It then enumerates
the device through the DDK. It exposes no rendering, private ioctl, mmap, or legacy
procfs interface. Firmware loading, GPU reset, command submission, and SGX IRQ
enablement stay disabled. The desktop continues to use llvmpipe.

The services module borrows the existing auxiliary device and register mapping;
it does not acquire PCI again or change display modes. Legacy DDK register
reads/writes are blocked and counted in this build. Successful preparation and
cleanup must both report **zero blocked hardware accesses**.

## Build on the Atom

From this repository's root:

```sh
git pull --ff-only
sudo apt install build-essential linux-headers-$(uname -r) dwarves python3 git patch
bash tests/run.sh
```

Use the clean pinned openpvrsgx checkout from the previous build, if available.
Otherwise create a persistent source cache (the directory below must be new):

```sh
sgx_source="$HOME/.cache/gma3600/openpvrsgx"
mkdir -p "$HOME/.cache/gma3600"
git init "$sgx_source"
git -C "$sgx_source" remote add origin https://github.com/openpvrsgx-devgroup/linux_openpvrsgx.git
git -C "$sgx_source" fetch --depth=1 --filter=blob:none origin 4000c3fb6577ecad4800e4fa2695777643bfaf86
git -C "$sgx_source" sparse-checkout init --cone
git -C "$sgx_source" sparse-checkout set drivers/gpu/drm/pvrsgx/1.17.4948957/eurasia_km
git -C "$sgx_source" checkout --detach FETCH_HEAD
```

Build using that path, or substitute your existing checkout path for `--source`:

```sh
python3 tools/sgx545_hybrid_build.py \
  --kdir /lib/modules/$(uname -r)/build \
  --source "$HOME/.cache/gma3600/openpvrsgx" \
  --sgx-rev 10131 \
  --output /tmp/gma3600-sgx-services1
modinfo /tmp/gma3600-sgx-services1/core/gma3600_sgx_services.ko
```

The output directory must be new. The tool applies pinned source patches, runs
mocked tests of the actual DDK reset-allocation and register-access functions,
and builds the modules. It never installs/loads them or runs proprietary libraries.
Check that vermagic matches `uname -r`. `10131` is the original Intel DDK build
profile, used here for compilation; this checkpoint does not validate firmware
compatibility. No i386 libraries or changes to your installed graphics stack are
needed yet.

## Load the services module with the working desktop

Keep SSH connected and stream `sudo dmesg -wH` in another terminal. Confirm:

```sh
cat /sys/module/gma3600/version
ls /sys/bus/auxiliary/devices/gma3600.sgx.*
```

The loaded parent must be `1.0.4-sgx-host2`, with the auxiliary device present.
If you are still running another parent version, use the
[late-loading procedure](../experimental/sgx545/README.md#test-identification-on-the-d2500hn)
to load the matching host build first. Do not forcibly unload the live display
driver. The following child-module test does not require a reboot when host2 is
already loaded.

If the identification module is loaded, release its auxiliary binding first:

```sh
sudo rmmod gma3600_sgx_probe
```

Then load the new module and read its status:

```sh
sudo insmod /tmp/gma3600-sgx-services1/core/gma3600_sgx_services.ko prepare=1
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo dmesg | grep -Ei 'gma3600|services|sgx|BUG:|Oops' | tail -100
```

Expected status resembles:

```text
state=prepared devices=1 pages_live=4 blocked_hw_accesses=0 gpu_execution=disabled
```

The page count must be at least four; the current profile normally allocates
one page directory and three reset-buffer pages. `state=prepared` and a bound
auxiliary driver are the success criteria. `insmod` returning successfully alone
does not prove that the device probe succeeded. A missing status file requires
the kernel logs, not repeated load attempts.

## Verify cleanup

```sh
sudo rmmod gma3600_sgx_services
sudo dmesg | grep -Ei 'gma3600|services|sgx|BUG:|Oops' | tail -100
```

Expected cleanup line:

```text
services cleanup: result=0 pages_live=0 blocked_hw_accesses=0
```

Return the status, cleanup line, any errors, and whether the desktop remained
usable. If all counts and results match, repeat the load/status/unload sequence
once to check a fresh module lifetime. The DDK globals allow one preparation
attempt per module load; reload the module for another attempt. If cleanup
reports retained pages or blocked accesses, stop and return the logs before
retrying. The allocator retains pages when write-back restoration fails rather
than returning RAM with conflicting cache attributes to the general allocator.

To restore the identification-only companion afterward, load the probe built
in the same output directory:

```sh
sudo insmod /tmp/gma3600-sgx-services1/probe/gma3600_sgx_probe.ko
```

## What this establishes

A passing result establishes that CPU-side services registration, low physical
page allocation, cache mappings, device enumeration, and teardown operate on the
amd64 target alongside KMS. It does not validate device DMA mappings, SGX IRQ
delivery, initializer/microkernel compatibility, or hardware rendering. The next
step is the controlled i386 bridge and matching userspace initialization.
