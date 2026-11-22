#!/usr/bin/env python3
"""Read-only Cedarview/SGX545 compatibility inventory; never opens GPU MMIO."""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import struct
import subprocess

LIBRARY_DIRS = (
    "usr/lib", "usr/lib/dri", "usr/lib/x86_64-linux-gnu",
    "usr/lib/x86_64-linux-gnu/dri", "usr/lib/i386-linux-gnu",
    "usr/lib/i386-linux-gnu/dri", "usr/local/lib", "usr/lib/cedarview",
    "usr/lib/pvr", "usr/lib/i386-linux-gnu/cedarview", "usr/lib/i386-linux-gnu/pvr",
    "usr/lib/x86_64-linux-gnu/pvr",
)
LIBRARY_PATTERNS = (
    "libGL.so*", "libGLX*.so*", "libEGL.so*", "libGLES*.so*", "libpvr*.so*", "libPVR*.so*",
    "libIMG*.so*", "libglslcompiler*.so*", "cedarview_dri.so",
)
MACHINES = {3: "i386", 62: "x86_64", 40: "ARM", 183: "AArch64"}


def elf_info(path):
    """Inspect a header without loading the library or running ldd."""
    try:
        with path.open("rb") as stream:
            header = stream.read(20)
    except OSError as exc:
        return {"status": "unreadable", "error": exc.strerror}
    if len(header) < 20 or header[:4] != b"\x7fELF":
        return {"status": "not_elf"}
    if header[4] not in (1, 2) or header[5] not in (1, 2):
        return {"status": "invalid_elf_header"}
    order = "little" if header[5] == 1 else "big"
    machine = int.from_bytes(header[18:20], order)
    return {"status": "elf", "bits": 32 if header[4] == 1 else 64,
            "machine": MACHINES.get(machine, f"ELF machine {machine}"),
            "compatibility": "not_verified"}


def read_optional(path):
    try:
        return path.read_text(errors="replace").strip()
    except OSError:
        return None


def resources(path):
    result = []
    text = read_optional(path)
    if text is None:
        return result
    for index, line in enumerate(text.splitlines()):
        try:
            start, end, flags = (int(word, 16) for word in line.split())
        except ValueError:
            result.append({"index": index, "status": "malformed"})
            continue
        size = end - start + 1 if start and end >= start else 0
        result.append({"index": index, "start": hex(start), "size": size,
                       "flags": hex(flags)})
    return result


def collect(root):
    devices = []
    pci_root = root / "sys/bus/pci/devices"
    drm_root = root / "sys/class/drm"
    for device in sorted(pci_root.glob("*")):
        try:
            vendor = int(read_optional(device / "vendor") or "", 16)
            device_id = int(read_optional(device / "device") or "", 16)
        except ValueError:
            continue
        if vendor != 0x8086 or not 0x0BE0 <= device_id <= 0x0BEF:
            continue
        driver = device / "driver"
        drm_nodes = []
        for node in sorted(drm_root.glob("*")):
            if re.fullmatch(r"(?:card|renderD)\d+", node.name):
                if (node / "device").resolve() == device.resolve():
                    drm_nodes.append(node.name)
        devices.append({
            "pci_address": device.name, "vendor": hex(vendor),
            "device": hex(device_id), "revision": read_optional(device / "revision"),
            "driver": driver.resolve().name if driver.is_symlink() else None,
            "resources": resources(device / "resource"), "drm_nodes": drm_nodes,
        })
    libraries = []
    seen = set()
    for directory in LIBRARY_DIRS:
        for pattern in LIBRARY_PATTERNS:
            for path in sorted((root / directory).glob(pattern)):
                if not path.is_file() or str(path) in seen:
                    continue
                seen.add(str(path))
                libraries.append({"path": "/" + str(path.relative_to(root)),
                                  **elf_info(path)})
    return {
        "schema_version": 1,
        "scope": "Read-only metadata; no MMIO, GPU reset, module loading, or library execution",
        "gma3600_version": read_optional(root / "sys/module/gma3600/version"),
        "devices": devices, "libraries": libraries,
        "assessment": {
            "sgx_revision": "not_probed",
            "firmware_compatibility": "not_verified",
            "userspace_abi_compatibility": "not_verified",
            "note": "A render node or library filename does not establish hardware acceleration.",
        },
    }


def command_output(command):
    try:
        result = subprocess.run(command, capture_output=True, text=True,
                                timeout=10, check=False)
        if result.returncode:
            return {"status": "failed", "exit_code": result.returncode}
        return {"status": "ok", "output": result.stdout.strip()}
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"status": "unavailable", "reason": type(exc).__name__}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("/"),
                        help="filesystem root; alternative roots are for offline fixtures")
    parser.add_argument("--glxinfo", action="store_true",
                        help="also query the current X session with glxinfo -B")
    parser.add_argument("--library", type=Path, action="append", default=[],
                        help="inspect an explicit candidate library's ELF header without loading it")
    args = parser.parse_args()
    root = args.root.resolve()
    report = collect(root)
    if root == Path("/"):
        report["host"] = {"kernel_release": platform.release(), "machine": platform.machine(),
                          "python_process_bits": struct.calcsize("P") * 8,
                          "debian_architecture": command_output(["dpkg", "--print-architecture"]),
                          "foreign_architectures": command_output(["dpkg", "--print-foreign-architectures"])}
        config = read_optional(root / "boot" / ("config-" + platform.release()))
        report["host"]["kernel_compat_config"] = (
            [line for line in config.splitlines()
             if line.startswith(("CONFIG_COMPAT=", "CONFIG_IA32_EMULATION=",
                                 "# CONFIG_COMPAT ", "# CONFIG_IA32_EMULATION "))]
            if config is not None else "unavailable")
    if args.library:
        report["candidate_libraries"] = [
            {"path": str(path), **elf_info(path)} for path in args.library]
    if args.glxinfo:
        if root != Path("/") or not os.environ.get("DISPLAY"):
            report["glxinfo"] = {"status": "skipped", "reason": "requires a live X session with DISPLAY"}
        else:
            query = command_output(["glxinfo", "-B"])
            if query["status"] == "ok":
                query["output"] = [line.strip() for line in query["output"].splitlines()
                                   if line.startswith(("OpenGL", "direct rendering:"))]
            report["glxinfo"] = query
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
