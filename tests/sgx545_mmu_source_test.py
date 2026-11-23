#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise actual patched DDK MMU functions and offline transaction."""
from pathlib import Path
import os
import re
import subprocess
import sys


def main():
    core, output = Path(sys.argv[1]), Path(sys.argv[2])
    root = Path(__file__).resolve().parents[1]
    source = (Path(sys.argv[3]) if len(sys.argv) > 3 else core / "services4/srvkm/devices/sgx/mmu.c").read_text()
    code = ""
    for name in ("_DeferredFreePageTable", "MMU_Alloc", "MMU_Free", "MMU_MapPage",
                 "MMU_MapScatter", "MMU_GetPhysPageAddr", "MMU_UnmapPagesAndFreePTs", "MMU_FreePageTables"):
        # Match definitions, not the forward declarations in mmu.c.
        match = re.search(r"(?:static )?(?:IMG_VOID|IMG_BOOL|IMG_DEV_PHYADDR)\s+" +
                          re.escape(name) + r"\s*\([^;{}]+\)\s*\{", source)
        if not match:
            raise RuntimeError(f"Missing pinned function definition: {name}")
        end = source.index("\n}", match.end()) + 2
        code += source[match.start():end] + "\n"
    diag = (root / "experimental/sgx545/core/gma3600_sgx_mmu_diag.inc").read_text()
    code += "\n".join(line for line in diag.splitlines() if not line.startswith("#include"))
    output.mkdir()
    (output / "mmu_functions.inc").write_text(code)
    for rev in ("10131", "10141"):
        binary = output / ("test-" + rev)
        subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-fsanitize=address,undefined", "-DGMA3600_SGX_HYBRID",
                        "-DLINUX", "-DSGX545", f"-DSGX_CORE_REV={rev}", "-I", str(output),
                        "-I", str(core / "include4"),
                        "-I", str(core / "services4/srvkm/hwdefs"),
                        "-I", str(root / "experimental/sgx545/core"),
                        "-I", str(root / "experimental/sgx545/abi"),
                        str(root / "tests/sgx545_mmu_source_test.c"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
