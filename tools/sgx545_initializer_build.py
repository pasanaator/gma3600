#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build a private i386 Intel-initializer diagnostic, without installing packages.

Downloads three exact, checksum-pinned Ubuntu artifacts if absent from cache.
Only compilation and ELF metadata inspection occur here. Proprietary code is
never loaded or executed by this builder, including on the cloud computer.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
INTEL = json.loads((ROOT / "research/sgx545/cedarview-20120717.json").read_text())["package"]
PACKAGES = [
    {**INTEL, "filename": "cedarview-graphics-drivers_20120717-0ubuntu1_i386.deb"},
    {"filename": "libc6_2.15-0ubuntu10.23_i386.deb", "size": 3941814,
     "sha256": "ad863081a45dd42701eb05c5ff8aeb09a0f4036bb85e35e02130cc24956d1753"},
    {"filename": "libc6-dev_2.15-0ubuntu10.23_i386.deb", "size": 5111720,
     "sha256": "a93db17cd08cbbf12a7f1761d4f7c6ba067f54038cd36c4e8a245eb8754d28f0"},
]
for package in PACKAGES[1:]:
    package["url"] = "https://old-releases.ubuntu.com/ubuntu/pool/main/e/eglibc/" + package["filename"]


def verify(path, metadata):
    if path.stat().st_size != metadata["size"] or hashlib.sha256(path.read_bytes()).hexdigest() != metadata["sha256"]:
        raise ValueError("Package size/SHA256 mismatch: " + path.name)


def obtain(cache, metadata, offline=False):
    path = cache / metadata["filename"]
    if not path.exists():
        if offline:
            raise ValueError("Missing cached package: " + str(path))
        # Bound downloads to the pinned package size before saving/extracting.
        with urllib.request.urlopen(metadata["url"], timeout=45) as response:
            data = response.read(metadata["size"] + 1)
        if len(data) != metadata["size"] or hashlib.sha256(data).hexdigest() != metadata["sha256"]:
            raise ValueError("Downloaded package size/SHA256 mismatch: " + path.name)
        path.write_bytes(data)
    verify(path, metadata)
    return path


def run(command):
    subprocess.run(command, check=True, env={**os.environ, "LC_ALL": "C"})


def build(output, cache, cc, offline=False):
    if output.exists() or output.is_symlink():
        raise ValueError("--output must be a new directory")
    cache.mkdir(parents=True, exist_ok=True)
    packages = []
    for metadata in PACKAGES:
        print("Verifying/downloading:", metadata["filename"], flush=True)
        packages.append(obtain(cache, metadata, offline))
    output.mkdir(parents=True)
    (output / "empty-home").mkdir()
    private = output / "sysroot"
    intel = output / "intel-package"
    lib = output / "lib"
    lib.mkdir()
    run(["dpkg-deb", "--extract", str(packages[0]), str(intel)])
    for package in packages[1:]:
        run(["dpkg-deb", "--extract", str(package), str(private)])
    source = intel / "usr/lib/intel-cdv"
    for name in ("libsrv_um", "libsrv_init"):
        shutil.copyfile(source / (name + ".so.1.7.862890"), lib / (name + ".so"))
    shutil.copyfile(intel / "usr/share/doc/cedarview-graphics-drivers/copyright", output / "INTEL-LICENSE.txt")
    runtime = private / "lib/i386-linux-gnu"
    include = private / "usr/include"
    flags = [cc, "-m32", "-std=c11", "-D_GNU_SOURCE", "-O2", "-Wall", "-Wextra", "-Werror",
             "-fno-stack-protector", "-nostdlib",
             "-isystem", str(include / "i386-linux-gnu"), "-isystem", str(include),
             "-idirafter", "/usr/include/i386-linux-gnu", "-idirafter", "/usr/include/x86_64-linux-gnu",
             "-idirafter", "/usr/include",
             "-I" + str(ROOT / "experimental/sgx545/abi")]
    libc = ["-L" + str(runtime), "-Wl,-rpath-link," + str(runtime),
            "-Wl,-rpath-link," + str(private / "lib"), "-Wl,--no-as-needed", "-l:libc.so.6"]
    run(flags + ["-fPIC", "-fvisibility=hidden", "-shared", "-Wl,-z,defs", "-Wl,-soname,libdrm.so.2",
                 str(ROOT / "tools/sgx545_initializer_transport.c"), *libc, "-o", str(lib / "libdrm.so.2")])
    run(flags + ["-fPIC", "-shared", "-Wl,-z,defs", str(ROOT / "tests/sgx545_initializer_fixture.c"),
                 "-L" + str(lib), "-l:libdrm.so.2", *libc, "-l:libpthread.so.0", "-l:librt.so.1",
                 "-o", str(lib / "libsgx545_initializer_fixture.so")])
    run(flags + ["-fno-pie", "-no-pie", str(ROOT / "tools/sgx545_initializer_start.S"),
                 str(ROOT / "tools/sgx545_initializer_main.c"), *libc, "-l:libdl.so.2",
                 "-Wl,--dynamic-linker,/lib/ld-linux.so.2", "-o", str(output / "sgx545-initializer")])
    artifacts = {}
    for path in [output / "sgx545-initializer", *sorted(lib.iterdir()),
                 *(runtime / name for name in ("ld-2.15.so", "libc.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1"))]:
        artifacts[str(path.relative_to(output))] = hashlib.sha256(path.read_bytes()).hexdigest()
    report = {"schema": 1, "transport_version": "0.1.0", "packages": PACKAGES,
              "artifact_sha256": artifacts, "proprietary_code_executed": False,
              "packages_installed": False, "kernel_or_display_driver_changed": False,
              "runtime_scope": "Normal-use initializer API trace through a private, limited libdrm transport; not a security sandbox or hardware execution test"}
    (output / "bundle.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Built private i386 initializer diagnostic:", output)
    print("No package installed and no proprietary code executed.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cache", type=Path, default=Path.home() / ".cache/gma3600/initializer-packages")
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    try:
        build(args.output.absolute(), args.cache.absolute(), args.cc, args.offline)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
