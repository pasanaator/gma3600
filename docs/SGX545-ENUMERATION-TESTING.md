# Original Intel enumeration compatibility

**Hardware result:** [both 0.9.1 tests passed on the D2500HN](../research/sgx545/d2500hn-enumeration.json).
Both enumeration forms agreed, all three device heaps passed, and unload released
every tracked page with no errors or blocked hardware accesses. The next step is
[capturing real Intel initializer requests](SGX545-INITIALIZER-CAPTURE.md).

Both initializer staging tests [passed on the Atom](../research/sgx545/d2500hn-initializer-staging.json).
The driver retained eleven allocations after creator close and released every
tracked page on unload, without errors or blocked register accesses.

Services **0.9.1** fixes another difference between Intel's original headers and
the community compatibility headers. The original enumeration reply is **328
bytes**, containing sixteen **20-byte identifiers**. The community reply is
200 bytes with 12-byte identifiers. The earlier audit measured neither type,
and the diagnostic used the compact reply even in Intel mode. Its previous
passes established the tested CONNECT/free and allocation paths only.

The bridge now translates both enumeration forms explicitly. Intel's two unused
PDump pointer slots are zero; native kernel pointers never enter the reply.
Only exact sizes are accepted. Tests cover zero, two and sixteen devices,
identifier stride, unused fields, output guards, backend errors and copy faults.
The i386 client requests both forms and compares their contents on every run.

The compiler audit now measures enumeration, acquisition, shared allocation/free,
mapping metadata, initializer-query inputs and heap sizes as well as its previous
fields. Its expanded manifest uses schema version 2; rebuild the core before
running the audit. Original and compatibility i386 declarations are checked
against the actual headers. This does not establish complete legacy library or
firmware compatibility. No proprietary library is executed by these checks.

## Test on Debian 13 amd64

Reuse the loaded `1.0.4-sgx-host2` display parent. Keep SSH connected and kernel
logs streaming in another terminal. From the repository root:

```sh
git pull --ff-only
bash tests/run.sh
sgx_build="$HOME/Desktop/gma3600-sgx-enumeration1"
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
sudo "$sgx_build/sgx545-compat-probe"
sudo "$sgx_build/sgx545-compat-probe" --device-intel
cat /sys/bus/auxiliary/devices/gma3600.sgx.*/services
sudo rmmod gma3600_sgx_services
sudo dmesg | grep -Ei 'SGX buffers|SGX bridge|services cleanup|blocked hardware|BUG:|Oops' | tail -10
```

Choose a new build directory if that one exists. Expect version `0.9.1` and
`ENUMERATE=328/200` in selftest. Both live client runs should include:

```text
PASS: Intel ENUMERATE=328 and compact ENUMERATE=200 agree; unused pointer slots are zero
```

The second run ends with `original Intel CONNECT=8 ENUMERATE=328
FREE_DEVICE_IN=60 FREE_DEVICE_OUT=8 payload forms`. It uses a fresh initializer
attempt: the preceding plain run does not consume that attempt. If another
initializer mode has already run, unload/reload services before `--device-intel`.

Expect `pages_live=4` before unload, `init_ran=1`, and zero resource/error counters
apart from the one registered device. Unload must report `result=0 pages_live=0
blocked_hw_accesses=0`. Return both client outputs and the final status/cleanup
lines. The existing staging modes need not be repeated for this change.

The parent display driver is unchanged. GPU execution remains disabled and
llvmpipe remains expected. The next execution milestone still requires genuine
initializer payloads, native BM/resource lifetime integration, firmware matching
and controlled hardware bring-up.
