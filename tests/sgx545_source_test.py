#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Test patched DDK functions and compile actual public layouts in i386 mode."""
from pathlib import Path
import os
import subprocess
import sys


def extract_function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    # These pinned functions use column-zero closing braces only at function end.
    end = text.index("\n}", brace) + 2
    return text[start:end] + "\n"


def main():
    core = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    root = Path(__file__).resolve().parents[1]
    mmu = (core / "services4/srvkm/devices/sgx/mmu.c").read_text()
    osfunc = (core / "services4/srvkm/env/linux/osfunc.c").read_text()
    output.mkdir()
    code = extract_function(mmu, "PVRSRV_ERROR MMU_BIFResetPDAlloc(")
    code += extract_function(osfunc, "IMG_UINT32 OSReadHWReg(")
    code += extract_function(osfunc, "IMG_VOID OSWriteHWReg(")
    (output / "sgx545_source_functions.inc").write_text(code)
    binary = output / "sgx545-source-test"
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=c11", "-Wall", "-Wextra",
                    "-Werror", "-fsanitize=undefined", "-DGMA3600_SGX_PREPARE_ONLY",
                    "-DGMA3600_SGX_HYBRID", "-I", str(core),
                    "-I", str(output), str(root / "tests/sgx545_reset_test.c"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    # Public headers need C99 integer/limit types and x86's asm-generic ioctl
    # definitions, not a 32-bit libc. USE_CODE skips an unused unistd include.
    includes = output / "freestanding"
    (includes / "asm").mkdir(parents=True)
    (includes / "limits.h").write_text(
        "#define INT_MAX __INT_MAX__\n#define INT_MIN (-__INT_MAX__ - 1)\n"
        "#define UINT_MAX (2U * __INT_MAX__ + 1U)\n")
    (includes / "asm/ioctl.h").write_text("#include <asm-generic/ioctl.h>\n")
    subprocess.run([os.environ.get("CC", "cc"), "-m32", "-ffreestanding", "-std=c11",
                    "-Wall", "-Wextra", "-Werror", "-DLINUX", "-DUSE_CODE",
                    "-DTRANSFER_QUEUE", "-DPVR_SECURE_HANDLES", "-DPVR_ABI_COMPAT=1,7,862890",
                    "-DSGX545", "-DSGX_CORE_REV=10131", "-DSUPPORT_PERCONTEXT_PB",
                    "-DSUPPORT_SGX_HWPERF", "-DSUPPORT_HW_RECOVERY",
                    "-I", str(includes),
                    "-I", str(core / "include4"), "-I", str(core / "services4/include"),
                    "-I", str(core / "services4/system/cedarview_linux"),
                    "-I", str(core / "services4/srvkm/hwdefs"),
                    "-I", str(core / "services4/srvkm/devices/sgx"),
                    "-I", str(root / "experimental/sgx545/abi"), "-c",
                    str(root / "tests/sgx545_ddk_i386_layout.c"),
                    "-o", str(output / "ddk-i386-layout.o")], check=True)
    print("Actual DDK i386 public-header layout and command assertions passed")
    subprocess.run([sys.executable, str(root / "tests/sgx545_init_source_test.py"),
                    str(core), str(output / "initializer")], check=True)
    subprocess.run([sys.executable, str(root / "tests/sgx545_mmu_source_test.py"),
                    str(core), str(output / "mmu")], check=True)
    subprocess.run([sys.executable, str(root / "tests/sgx545_bm_source_test.py"),
                    str(core), str(output / "bm")], check=True)
    subprocess.run([sys.executable, str(root / "tests/sgx545_native_source_test.py"),
                    str(core), str(output / "native")], check=True)


if __name__ == "__main__":
    main()
