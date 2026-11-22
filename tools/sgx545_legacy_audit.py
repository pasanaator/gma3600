#!/usr/bin/env python3
"""Checksum and inspect the pinned Intel Cedarview package without executing it."""
import argparse
from email.parser import Parser
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

from sgx545_inventory import elf_info

MANIFEST = Path(__file__).resolve().parents[1] / "research/sgx545/cedarview-20120717.json"
ENTRYPOINTS = {
    "eglGetDisplay", "eglInitialize", "eglChooseConfig", "eglBindAPI",
    "eglCreatePbufferSurface", "eglCreateContext", "eglMakeCurrent",
    "eglGetProcAddress", "glClear", "glClearColor", "glReadPixels", "glFinish",
    "glGetString", "glDrawArrays", "PVRSRVConnect", "PVRSRVEnumerateDevices", "SrvInit",
}


def command(args):
    result = subprocess.run(args, capture_output=True, text=True, check=True,
                            timeout=30, env={**os.environ, "LC_ALL": "C"})
    return result.stdout


def verify_package(path, metadata):
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
            size += len(chunk)
    if size != metadata["size"] or digest.hexdigest() != metadata["sha256"]:
        raise ValueError("Package size/SHA256 does not match the pinned Ubuntu artifact; refusing extraction")
    return digest.hexdigest()


def parse_dynamic(text):
    needed = re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", text)
    soname = re.search(r"\(SONAME\).*?\[([^\]]+)\]", text)
    return needed, soname.group(1) if soname else None


def parse_exports(text):
    exports = set()
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 8 and fields[0].rstrip(":").isdigit():
            if fields[4] in ("GLOBAL", "WEAK") and fields[6] != "UND":
                exports.add(fields[7].split("@", 1)[0])
    return sorted(exports & ENTRYPOINTS)


def parse_versions(text):
    requirements = {}
    current = None
    in_needs = False
    for line in text.splitlines():
        if line.startswith("Version needs section"):
            in_needs = True
        elif line.startswith("Version ") and "section" in line:
            in_needs = False
        if not in_needs:
            continue
        match = re.search(r"File:\s+(\S+)", line)
        if match:
            current = match.group(1)
            requirements.setdefault(current, [])
        match = re.search(r"Name:\s+(\S+)", line)
        if match and current:
            requirements[current].append(match.group(1))
    return requirements


def dependency_closure(libraries, roots):
    providers = {}
    for lib in libraries:
        for name in lib["aliases"]:
            providers[name] = lib
    pending = list(roots)
    visited = set()
    external = set()
    internal = set()
    while pending:
        name = pending.pop()
        if name in visited:
            continue
        visited.add(name)
        lib = providers.get(name)
        if lib is None:
            external.add(name)
        else:
            if lib["path"] not in internal:
                internal.add(lib["path"])
                pending.extend(lib["needed"])
    return {"roots": roots, "internal_libraries": sorted(internal),
            "external_sonames": sorted(external),
            "scope": "DT_NEEDED only; dynamically selected WSEGL/compiler/init modules are not covered"}


def audit_package(package, manifest):
    package = package.resolve()
    sha256 = verify_package(package, manifest["package"])
    control = Parser().parsestr(command(["dpkg-deb", "--field", str(package)]))
    for field, key in (("Package", "name"), ("Version", "version"), ("Architecture", "architecture")):
        if control.get(field) != manifest["package"][key]:
            raise ValueError("Package control metadata does not match the pinned artifact")
    libraries = []
    with tempfile.TemporaryDirectory(prefix="sgx545-metadata-") as folder:
        root = Path(folder)
        # No installation or maintainer script execution; hash checked first.
        command(["dpkg-deb", "--extract", str(package), str(root)])
        aliases = {}
        files = sorted(root.rglob("*"))
        for path in files:
            if path.is_symlink():
                target = os.readlink(path)
                aliases.setdefault(Path(target).name, []).append(path.name)
        for path in files:
            if path.is_symlink() or not path.is_file():
                continue
            ident = elf_info(path)
            if ident.get("status") != "elf":
                continue
            needed, soname = parse_dynamic(command(["readelf", "--dynamic", "--wide", str(path)]))
            names = set(aliases.get(path.name, [])) | {path.name}
            if soname:
                names.add(soname)
            libraries.append({
                "path": str(path.relative_to(root)), **ident,
                "soname": soname, "aliases": sorted(names), "needed": needed,
                "exported_test_entrypoints": parse_exports(command(["readelf", "--dyn-syms", "--wide", str(path)])),
                "symbol_versions_needed": parse_versions(command(["readelf", "--version-info", "--wide", str(path)])),
            })
    egl = next((lib for lib in libraries if "libEGL.so" in lib["aliases"]), None)
    return {
        "schema_version": 1, "package": {"name": control["Package"], "version": control["Version"],
            "architecture": control["Architecture"], "sha256": sha256,
            "depends": control.get("Depends"), "pre_depends": control.get("Pre-Depends")},
        "library_count": len(libraries), "libraries": libraries,
        "egl_pbuffer_export_present": bool(egl and "eglCreatePbufferSurface" in egl["exported_test_entrypoints"]),
        "offscreen_static_dependencies": dependency_closure(libraries, ["libEGL.so", "libGLESv2.so"]),
        "runtime_validation": "not_performed",
        "scope": "Static metadata only. No library loading, ldd, disassembly, GPU calls, installation, or maintainer scripts.",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path, help="downloaded cedarview-graphics-drivers i386 .deb")
    parser.add_argument("--output", type=Path, help="write the JSON report here instead of stdout")
    args = parser.parse_args()
    try:
        report = audit_package(args.package, json.loads(MANIFEST.read_text()))
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        parser.exit(2, f"Audit failed: {exc}\n")
    text = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
