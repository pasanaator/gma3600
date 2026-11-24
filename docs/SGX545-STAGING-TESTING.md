# Initializer translation and ownership handoff

The [original Intel payload test passed on the Atom](../research/sgx545/d2500hn-intel-abi.json),
and the supplied compiler report matches the expected i386/amd64 layouts.
Services **0.9.0** adds a project-only initializer staging interface. It translates
the 2,068-byte i386 request to native DDK metadata and hands the referenced
allocations to a driver-owned stage, which can outlive its creating file.

**GPU execution remains disabled.** Legacy initializer command 99 still returns
`EOPNOTSUPP`. The native memory wrappers are private metadata snapshots, without
native BM/resource-manager registration; they are never passed to the native
initializer, free, power or submission routines. The diagnostic uses zeroed
buffers and HALT-only scripts, not a firmware image. llvmpipe remains expected.

The code validates nine required roles and up to eighteen optional distinct
handles, file ownership, live native PTEs, size bounds and kernel-known role
minimums. It checks build options through the DDK mask, and the host-control and
command structure sizes. The other eleven size declarations are bounded and
preserved; they remain unverified against firmware. Scripts need supported
opcodes, aligned offsets within the SGX window and a HALT in each bounded array.
Unused script tails are zeroed in the snapshot. These checks do not approve a
register script for hardware execution.

Ownership transfer is atomic, and failed result copyout restores the creating
file's ownership. A successful handoff ends the one-shot initializer attempt
with native SUCCESSFUL still false. Existing CPU VMAs remain usable, while the
creator's mapping/free tokens are revoked. Buffer contents are not sealed and
can still change through existing VMAs; this is another reason the stage is not
an executable firmware handoff. A same-process, privileged control connection
can inspect or abort the stage. Other processes are rejected. Driver detach
removes native PTEs before releasing backing pages and common DDK state.

The per-owner limit rises from 16 to **32** to accommodate the 27 possible
references. Global limits remain 64 live buffers and 16 MiB. A stage holds only
its referenced allocations; unreferenced file allocations are released normally.

## Build

Use the same pinned source cache and loaded host2 parent as before:

```sh
git pull --ff-only
bash tests/run.sh
sgx_build="$HOME/Desktop/gma3600-sgx-stage1"
python3 tools/sgx545_hybrid_build.py \
  --kdir /lib/modules/$(uname -r)/build \
  --source "$HOME/.cache/gma3600/openpvrsgx" \
  --sgx-rev 10131 \
  --output "$sgx_build"
modinfo -F version "$sgx_build/core/gma3600_sgx_services.ko"
"$sgx_build/sgx545-compat-probe" --selftest
```

Choose a new output directory if that one exists. Expected module version is
`0.9.0`; selftest includes `stage=32 control=24`. The builder runs the actual
translator against real native DDK structures with an OS type shim, for both
compile profiles, in addition to the existing MMU and layout tests. Cloud tests
never load a module or execute the GPU.

## Explicit stage abort

Keep SSH connected and `sudo dmesg -wH` streaming in another terminal. Reuse the
working `1.0.4-sgx-host2` parent. If absent, follow the existing
[late-load instructions](../experimental/sgx545/README.md#test-identification-on-the-d2500hn).

```sh
cat /sys/module/gma3600/version
if [ -d /sys/module/gma3600_sgx_services ]; then sudo rmmod gma3600_sgx_services; fi
if [ -d /sys/module/gma3600_sgx_probe ]; then sudo rmmod gma3600_sgx_probe; fi
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1
sudo "$sgx_build/sgx545-compat-probe" --init-stage
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
sudo dmesg | grep -Ei 'SGX buffers|SGX bridge|services cleanup|blocked hardware|BUG:|Oops' | tail -10
```

The client allocates nine role buffers, two additional buffers and one buffer
that is deliberately not handed off. It tests malformed payloads, duplicate and
unknown tokens, build/size/script checks, and copyout rollback. It then hands off
eleven buffers, verifies creator-token revocation and process isolation, closes
the creator and its last VMA, queries the still-live PTEs/native metadata through
the control connection, and aborts the stage.

Expected final line:

```text
PASS: i386 initializer staging, native snapshot, ownership rollback, creator-close lifetime and explicit staged abort
```

Before unload, expect `pages_live=4`, `init_running=0`, `init_ran=1`, and zero
`stages`, `staged_buffers`, `staged_pages`, `stage_errors`, file/connection/heap,
device-allocation and buffer/map/error counters. `blocked_hw_accesses` must be
zero throughout. Unload must report `result=0 pages_live=0 blocked_hw_accesses=0`.

## Keep the stage until module unload: fresh services load

```sh
sudo insmod "$sgx_build/core/gma3600_sgx_services.ko" prepare=1 bridge=1
sudo "$sgx_build/sgx545-compat-probe" --init-stage-close
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
sudo dmesg | grep -Ei 'SGX buffers|SGX bridge|services cleanup|blocked hardware|BUG:|Oops' | tail -10
```

Expected client ending: `driver-owned retention for unload`. After the client
exits, **one stage and eleven allocations intentionally remain**. Expect
`stages=1`, `staged_buffers=11`, `device_allocations=11`, `buffers=11`, and the
`staged_pages` value printed by the client. `device_pages` and `buffer_pages`
match that value. `pages_live` is larger because it also includes native page
tables and the four baseline pages. Files, connections, heap handles, buffer
VMAs, initialization-running and error counters must be zero; `init_ran=1`.

Unloading the services child must release that stage: the stopped bridge and
buffer lines must show zero resources, followed by `result=0 pages_live=0
blocked_hw_accesses=0`. Do not force removal if the module is still in use.
Return both client outputs, status and final cleanup lines, and whether the
desktop remained usable. The deliberately invalid device-index message from the
ordinary bridge checks remains expected. On any failure, retain logs/status and
stop the test; cleanup errors retain ownership rather than freeing unsafe RAM.
