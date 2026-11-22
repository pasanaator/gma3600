#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile/run three actual patched DDK functions against the mock OS fixture."""
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


if __name__ == "__main__":
    main()
