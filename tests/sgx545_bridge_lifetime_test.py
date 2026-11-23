#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise actual bridge file/stop functions with mock kernel/DDK lifetimes."""
from pathlib import Path
import os
import subprocess
import tempfile
from sgx545_source_test import extract_function


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "experimental/sgx545/core/gma3600_sgx_bridge.c").read_text()
    code = ""
    for name in ("gma_sgx_bridge", "gma_sgx_file"):
        start = source.index(f"struct {name} {{")
        code += source[start:source.index("\n};", start) + 3] + "\n"
    for signature in (
        "static int new_token(", "static int backend_connect(",
        "static int backend_disconnect(", "static void bridge_free(",
        "static void bridge_file_free(", "static int bridge_open(",
        "static int bridge_release(", "static void bridge_stop(",
        "int gma3600_sgx_bridge_register(",
    ):
        code += extract_function(source, signature)
    with tempfile.TemporaryDirectory(prefix="sgx-bridge-lifetime-") as directory:
        output = Path(directory)
        (output / "bridge_functions.inc").write_text(code)
        binary = output / "test"
        subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-Wall",
                        "-Wextra", "-Werror", "-Wno-unused-parameter",
                        "-fsanitize=address,undefined", "-I", str(output),
                        "-I", str(root / "experimental/sgx545/abi"),
                        str(root / "tests/sgx545_bridge_lifetime_test.c"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
