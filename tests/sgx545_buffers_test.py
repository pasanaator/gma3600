#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the actual buffer provider with mock page and VMA APIs."""
from pathlib import Path
import os
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "experimental/sgx545/core/gma3600_sgx_buffers.c").read_text()
    with tempfile.TemporaryDirectory(prefix="sgx-buffers-") as directory:
        output = Path(directory)
        (output / "buffer_provider.inc").write_text("\n".join(
            line for line in source.splitlines() if not line.startswith("#include")))
        binary = output / "test"
        subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-Wall",
                        "-Wextra", "-Werror", "-Wno-unused-parameter",
                        "-fsanitize=address,undefined", "-I", str(output),
                        "-I", str(root / "experimental/sgx545/abi"),
                        "-I", str(root / "experimental/sgx545/core"),
                        str(root / "tests/sgx545_buffers_test.c"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
