#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run actual adapter lease/refcount and common-cleanup functions with OS mocks."""
from pathlib import Path
import os
import subprocess
import tempfile
from sgx545_source_test import extract_function


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "experimental/sgx545/core/gma3600_sgx_adapter.c").read_text()
    start = source.index("struct gma_sgx_prepared {")
    code = source[start:source.index("\n};", start) + 3]
    code += "\nstatic struct gma_sgx_prepared *active;\n"
    for signature in ("int gma3600_sgx_owner_acquire(", "int gma3600_sgx_owner_release(", "int gma3600_sgx_owner_seal(",
                      "static void gma_sgx_prepare_cleanup("):
        code += extract_function(source, signature)
    with tempfile.TemporaryDirectory(prefix="sgx-owner-adapter-") as directory:
        output = Path(directory)
        (output / "owner_functions.inc").write_text(code)
        binary = output / "test"
        subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-I", str(output),
                        str(root / "tests/sgx545_owner_adapter_test.c"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
