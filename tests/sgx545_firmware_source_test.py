#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the actual native reset and first-kick code with simulated MMIO.

No GPU access, module load, or proprietary library execution occurs here.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
from sgx545_source_test import extract_function


def main():
    core, output = Path(sys.argv[1]), Path(sys.argv[2])
    root = Path(__file__).resolve().parents[1]
    output.mkdir()
    shutil.copytree(core / "include4", output / "include4")
    types = output / "include4/img_types.h"
    types.write_text(types.read_text().replace('"kernel_types.h"', '"gma_test_kernel_types.h"'))
    (output / "include4/gma_test_kernel_types.h").write_text("#include <stddef.h>\n#include <stdint.h>\n#include <limits.h>\n")
    (output / "device.h").write_text("/* Device container unused by this fixture. */\n")
    reset = (core / "services4/srvkm/devices/sgx/sgxreset.c").read_text()
    # Keep preprocessor branches and the complete reset helpers, not a rewrite.
    reset = "\n".join(line for line in reset.splitlines() if not line.startswith("#include"))
    utils = (core / "services4/srvkm/devices/sgx/sgxutils.c").read_text()
    reset = extract_function(utils, "IMG_VOID SGXWaitClocks(") + reset
    init = (core / "services4/srvkm/devices/sgx/sgxinit.c").read_text()
    reset += extract_function(init, "static PVRSRV_ERROR SGXRunScript(")
    reset += extract_function(init, "PVRSRV_ERROR SGXInitialise(")
    (output / "firmware_functions.inc").write_text(reset)
    for rev in ("10131", "10141"):
        # Native profiles use multiple contexts. Also exercise the dormant
        # single-context invalidation wait so a timeout cannot become success.
        for single in (False, True):
            command = [os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                       "-fsanitize=address,undefined", "-D__KERNEL__", "-DLINUX", "-I", str(output),
                       "-I", str(output / "include4"), "-DTRANSFER_QUEUE", "-DPVR_SECURE_HANDLES",
                       "-DPVR_ABI_COMPAT=1,7,862890", "-DSGX545", "-DSGX_CORE_REV=" + rev,
                       "-DSUPPORT_PERCONTEXT_PB", "-DSUPPORT_SGX_HWPERF", "-DSUPPORT_HW_RECOVERY",
                       "-DSUPPORT_SGX_LOW_LATENCY_SCHEDULING", "-DSUPPORT_SGX_NEW_STATUS_VALS"]
            if single:
                command += ["-DTEST_SINGLE_CONTEXT"]
            for path in (core / "include4", core / "services4/include", core / "services4/srvkm/include",
                         core / "services4/srvkm/hwdefs", core / "services4/srvkm/devices/sgx",
                         core / "services4/system/cedarview_linux"):
                command += ["-I", str(path)]
            binary = output / ("test-" + rev + ("-single" if single else ""))
            subprocess.run(command + [str(root / "tests/sgx545_firmware_source_test.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    main()
