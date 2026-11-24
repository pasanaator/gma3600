#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the actual translator against real DDK structures with pointer handles."""
from pathlib import Path
import os
import shutil
import subprocess
import sys


def main():
    core, output = Path(sys.argv[1]), Path(sys.argv[2])
    root = Path(__file__).resolve().parents[1]
    output.mkdir()
    shutil.copytree(core / "include4", output / "include4")
    types = output / "include4/img_types.h"
    types.write_text(types.read_text().replace('"kernel_types.h"', '"gma_test_kernel_types.h"'))
    (output / "include4/gma_test_kernel_types.h").write_text("#include <stddef.h>\n#include <stdint.h>\n#include <limits.h>\n")
    for rev in ("10131", "10141"):
        binary = output / ("test-" + rev)
        # Only the OS type shim is mocked. Real DDK headers select their native
        # pointer-handle branches, with compiler assertions against Kbuild sizes.
        command = [os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-D__KERNEL__", "-DLINUX", "-I", str(output / "include4"), "-DTRANSFER_QUEUE", "-DPVR_SECURE_HANDLES",
                   "-DPVR_ABI_COMPAT=1,7,862890", "-DSGX545", "-DSGX_CORE_REV=" + rev,
                   "-DSUPPORT_PERCONTEXT_PB", "-DSUPPORT_SGX_HWPERF", "-DSUPPORT_HW_RECOVERY",
                   "-DSUPPORT_SGX_LOW_LATENCY_SCHEDULING", "-DSUPPORT_SGX_NEW_STATUS_VALS"]
        for path in (core / "include4", core / "services4/include", core / "services4/srvkm/hwdefs",
                     core / "services4/srvkm/devices/sgx", core / "services4/system/cedarview_linux", root / "experimental/sgx545/abi",
                     root / "experimental/sgx545/core"):
            command += ["-I", str(path)]
        command += [str(root / "tests/sgx545_init_source_test.c"),
                    str(root / "experimental/sgx545/core/gma3600_sgx_init.c"), "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
