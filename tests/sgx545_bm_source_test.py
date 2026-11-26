#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise native buffer-manager bodies with real DDK types and OS/MMU seams."""
from pathlib import Path
import os
import re
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
    (output / "device.h").write_text("/* Outer device container supplied by the fixture. */\n")
    osfunc = (core / "services4/srvkm/include/osfunc.h").read_text()
    (output / "os_seam.h").write_text("\n".join(re.findall(r"^#define PVRSRV_OS_(?:NON_)?PAGEABLE_HEAP[^\n]+", osfunc, re.M)))
    header = (core / "services4/srvkm/include/buffer_manager.h").read_text()
    (output / "buffer_manager.h").write_text(header.replace('#include "ra.h"', "").replace('#include "perproc.h"', ""))
    source = (core / "services4/srvkm/common/buffer_manager.c").read_text()
    code = ""
    # Actual native allocation/map/free/getter bodies. OS registration and MMU
    # callbacks are seams; the BM control flow and native data types are real.
    for name in ("DevMemoryAlloc", "DevMemoryFree", "WrapMemory", "FreeBuf", "BM_Free",
                 "BM_HandleToCpuVaddr", "BM_HandleToDevVaddr", "BM_HandleToOSMemHandle", "BM_GetPhysPageAddr"):
        match = re.search(r"(?:static\s+)?(?:IMG_BOOL|IMG_VOID|IMG_CPU_VIRTADDR|IMG_DEV_VIRTADDR|IMG_HANDLE)\s+" + name + r"\s*\([^;{}]+\)\s*\{", source)
        if not match: raise RuntimeError("Missing BM body: " + name)
        code += source[match.start():source.index("\n}", match.end()) + 2] + "\n"
    code += "\n".join(line for line in (root / "experimental/sgx545/core/gma3600_sgx_bm.inc").read_text().splitlines() if not line.startswith("#include"))
    (output / "bm_functions.inc").write_text(code)
    for rev in ("10131", "10141"):
        command = [os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-D__KERNEL__", "-DLINUX", "-I", str(output), "-I", str(output / "include4"),
                   "-DTRANSFER_QUEUE", "-DPVR_SECURE_HANDLES", "-DPVR_ABI_COMPAT=1,7,862890", "-DSGX545", "-DSGX_CORE_REV=" + rev,
                   "-DSUPPORT_PERCONTEXT_PB", "-DSUPPORT_SGX_HWPERF", "-DSUPPORT_HW_RECOVERY", "-DSUPPORT_SGX_LOW_LATENCY_SCHEDULING", "-DSUPPORT_SGX_NEW_STATUS_VALS"]
        for path in (core / "include4", core / "services4/include", core / "services4/srvkm/include", core / "services4/srvkm/hwdefs",
                     core / "services4/srvkm/devices/sgx", core / "services4/system/cedarview_linux", root / "experimental/sgx545/abi", root / "experimental/sgx545/core"):
            command += ["-I", str(path)]
        binary = output / ("test-" + rev)
        subprocess.run(command + [str(root / "tests/sgx545_bm_source_test.c"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
