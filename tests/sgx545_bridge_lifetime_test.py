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
    for name in ("gma_sgx_bridge", "gma_sgx_file", "mmu_visit_context", "gma_sgx_device_allocation", "device_visit_context", "gma_sgx_stage"):
        start = source.index(f"struct {name} {{")
        code += source[start:source.index("\n};", start) + 3] + "\n"
    code += "static void bridge_free(struct kref *ref);\n"
    init = (root / "experimental/sgx545/core/gma3600_sgx_init.c").read_text()
    code += extract_function(init, "static void wire_roles(")
    code += extract_function(init, "int gma_sgx_init_tokens(")
    for signature in (
        "static int new_token(", "static int backend_connect(",
        "static int backend_init_connect(", "static int backend_init_abort(",
        "static int backend_release_init_info(", "static int backend_init_info(",
        "static struct gma_sgx_device_allocation **device_slot(",
        "static int device_visit(", "static int device_free_owned(", "static int backend_free_device(",
        "static int backend_alloc_device(", "static int backend_free_shared(",
        "static int bridge_device_check(", "static int stage_requirements(",
        "static int stage_prepare(", "static int stage_validate(", "static void stage_drop_metadata(", "static int stage_commit(",
        "static int stage_rollback(", "static int stage_publish(", "static int stage_abort(",
        "static int stage_control(", "static int stage_content(", "static int backend_disconnect(", "static void bridge_free(struct kref *ref)\n{",
        "static void bridge_file_free(", "static int bridge_open(",
        "static int bridge_release(", "static void bridge_stop(",
        "int gma3600_sgx_bridge_register(",
        "static int bridge_mmu_visit(", "static int bridge_mmu_check(",
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
                        "-I", str(root / "experimental/sgx545/core"),
                        str(root / "tests/sgx545_bridge_lifetime_test.c"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
