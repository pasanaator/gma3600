#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compare verified Intel i386 initializer layouts with a built hybrid core.

Compiles layout constants and reads ELF sections. Never executes the resulting
objects, installs packages, loads modules, or runs proprietary libraries.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys

from sgx545_legacy_audit import verify_package

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = {
    "name": "cedarview-drm_20120717-0ubuntu1_i386.deb",
    "size": 2539960,
    "sha256": "835a324bb38a4d8c3957781cd7cae00dcaaaba1e11f8f5f042215cdea72b6933",
    "url": "https://old-releases.ubuntu.com/ubuntu/pool/multiverse/c/cedarview-drm-drivers/cedarview-drm_20120717-0ubuntu1_i386.deb",
}
SECTION = ".gma_sgx_abi"
# Release ABI features in the verified package's staging/cdv/Makefile.
REFERENCE_DEFINES = [
    "LINUX", "SGX545", "SUPPORT_SGX545", "SGX_CORE_REV=10131", "RELEASE",
    "SERVICES4", "PVR2D_VALIDATE_INPUT_PARAMS", "SUPPORT_DRI_DRM", "SUPPORT_DRI_DRM_EXT",
    "SUPPORT_SRVINIT", "SUPPORT_SGX", "SUPPORT_LINUX_X86_WRITECOMBINE", "TRANSFER_QUEUE",
    "SYS_USING_INTERRUPTS", "SUPPORT_HW_RECOVERY", "PVR_SECURE_HANDLES", "USE_PTHREADS",
    "SUPPORT_SGX_EVENT_OBJECT", "SUPPORT_SGX_HWPERF", "SUPPORT_LINUX_X86_PAT",
    "PVR_PROC_USE_SEQ_FILE", "SUPPORT_CACHE_LINE_FLUSH", "SUPPORT_CPU_CACHED_BUFFERS",
    "DISABLE_PM", "SUPPORT_SGX_NEW_STATUS_VALS", "SUPPORT_PERCONTEXT_PB",
    "DRM_PVR_USE_INTEL_FB", "PVRSRV_RESOURCE_PROFILING", "PERPROC_LIST", "RES_MAN_EXTEND",
    "PVRSRV_OS_MEM_INFO",
]
EXCLUDED_FROM_WIRE = {"pointer_bytes", "core_revision_profile", "build_options", "build_option_mask"}


def field_names():
    fields = ROOT / "experimental/sgx545/abi/sgx545_init_abi_fields.inc"
    return re.findall(r"^SGX545_ABI_VALUE\((\w+),", fields.read_text(), re.M)


def decode_manifest(data, names):
    if len(data) != len(names) * 4:
        raise ValueError("Incorrect ABI section length")
    result = dict(zip(names, struct.unpack("<" + "I" * len(names), data)))
    if result.get("magic") != 0x53475841 or result.get("version") != 2:
        raise ValueError("Unknown ABI manifest magic/version")
    return result


def public_defines(command):
    # Treat saved Kbuild text as data only; never execute its compiler or shell.
    first_line = command.splitlines()[0].split(":=", 1)[1]
    return [word[2:] for word in shlex.split(first_line)
            if word.startswith("-D") and word[2:] not in {"__KERNEL__", "MODULE", "CC_USING_FENTRY"}
            and not word[2:].startswith(("KBUILD_", "__KBUILD_"))]


def differences(reference, other, names):
    return {name: {"reference": reference[name], "other": other[name]}
            for name in names if reference[name] != other[name]}


def compare(reference, compat, native):
    wire = differences(reference, compat, sorted(set(reference) - EXCLUDED_FROM_WIRE))
    supported_aliases = {}
    for field, wire_field, original_bytes, compat_bytes in (
        ("connect_reply_bytes", "wire_connect_reply_bytes", 8, 12),
        ("enumerate_reply_bytes", "wire_enumerate_reply_bytes", 328, 200),
        ("device_identifier_bytes", "wire_device_identifier_bytes", 20, 12),
        ("device_free_request_bytes", "wire_device_free_request_bytes", 60, 12),
        ("device_free_reply_bytes", "wire_device_free_reply_bytes", 8, 4),
    ):
        if (reference.get(field) == original_bytes and compat.get(field) == compat_bytes and
                all(manifest.get(wire_field) == original_bytes for manifest in (reference, compat, native))):
            supported_aliases[field] = wire.get(field)
    unsupported_wire = {name: value for name, value in wire.items() if name not in supported_aliases}
    firmware_names = [name for name in reference if name.startswith("firmware_")]
    firmware = differences(reference, native, firmware_names)
    raw_options = reference["build_options"] ^ native["build_options"]
    masked_options = raw_options & native["build_option_mask"]
    return {
        "selected_i386_wire_layout_matches": not wire,
        "i386_wire_differences": wire,
        "supported_protocol_forms": supported_aliases,
        "selected_i386_wire_supported": not unsupported_wire,
        "unsupported_i386_wire_differences": unsupported_wire,
        "selected_firmware_control_layouts_match": not firmware,
        "firmware_control_differences": firmware,
        "raw_build_option_xor": raw_options,
        "masked_build_option_xor": masked_options,
        "masked_build_options_match": not masked_options,
        "native_requires_translation": bool(differences(compat, native, [
            name for name in reference if name.startswith("init2_") or name == "bridge_package_bytes"])),
        "native_translation_differences": differences(compat, native, [
            name for name in reference if name.startswith("init2_") or name == "bridge_package_bytes"]),
        "firmware_revision_or_execution_verified": False,
    }


def run(command, **kwargs):
    return subprocess.run(command, check=True, env={**os.environ, "LC_ALL": "C"}, **kwargs)


def read_section(objcopy, obj, directory, stem):
    binary = directory / (stem + ".bin")
    # Explicit output object: objcopy must not replace the input build artifact.
    run([objcopy, "--dump-section", SECTION + "=" + str(binary), str(obj),
         str(directory / (stem + "-copy.o"))])
    return decode_manifest(binary.read_bytes(), field_names())


def compile_public(cc, objcopy, core, defines, directory, stem, original=False):
    includes = directory / "freestanding"
    includes.mkdir(exist_ok=True)
    (includes / "asm").mkdir(exist_ok=True)
    (includes / "asm/ioctl.h").write_text("#include <asm-generic/ioctl.h>\n")
    (includes / "limits.h").write_text(
        "#define INT_MAX __INT_MAX__\n#define INT_MIN (-__INT_MAX__-1)\n"
        "#define UINT_MAX (2U*__INT_MAX__+1U)\n")
    # Public headers only reference libc prototypes in unused helper paths.
    (includes / "unistd.h").write_text("/* No libc functions used by the layout probe. */\n")
    command = [cc, "-m32", "-std=c11", "-ffreestanding", "-Wall", "-Wextra", "-Werror",
               "-I" + str(includes), "-I" + str(ROOT / "experimental/sgx545/abi")]
    for subdir in ("include4", "services4/include", "services4/srvkm/hwdefs",
                   "services4/srvkm/devices/sgx", "services4/system/unified",
                   "services4/system/cedarview_linux"):
        command.append("-I" + str(core / subdir))
    command += ["-D" + define for define in defines]
    if original:
        command.append("-DSGX545_ORIGINAL_INTEL")
    obj = directory / (stem + ".o")
    command += ["-c", str(ROOT / "experimental/sgx545/core/gma3600_sgx_abi_manifest.c"), "-o", str(obj)]
    run(command)
    # Run the same command/offset assertions against the original Intel headers
    # and the compatibility headers, not merely against two generated tables.
    assertion_flags = ["-DSGX545_ORIGINAL_INTEL"] if original else []
    run(command[:-4] + assertion_flags + ["-c", str(ROOT / "tests/sgx545_ddk_i386_layout.c"),
                                         "-o", str(directory / (stem + "-assertions.o"))])
    manifest = read_section(objcopy, obj, directory, stem)
    if manifest["pointer_bytes"] != 4:
        raise ValueError("Expected a genuine i386 compilation")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--intel-package", type=Path, required=True)
    parser.add_argument("--compat-core", type=Path, required=True, help="core/ from a new hybrid build")
    parser.add_argument("--output", type=Path, required=True, help="new output directory")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--objcopy", default="objcopy")
    args = parser.parse_args()
    try:
        verify_package(args.intel_package, PACKAGE)
        core = args.compat_core.resolve()
        obj = core / "gma3600_sgx_abi_manifest.o"
        cmd = core / ".gma3600_sgx_abi_manifest.o.cmd"
        if not obj.is_file() or not cmd.is_file():
            parser.error("--compat-core needs a build containing the ABI manifest object; rebuild with the current helper")
        header = obj.read_bytes()[:20]
        if header[:6] != b"\x7fELF\x02\x01" or int.from_bytes(header[18:20], "little") != 62:
            raise ValueError("Expected a native x86-64 kernel ABI object")
        output = args.output.absolute()
        if output.exists() or output.is_symlink():
            parser.error("--output must be a new directory")
        output.mkdir(parents=True)
        extracted = output / "reference-package"
        run(["dpkg-deb", "--extract", str(args.intel_package.resolve()), str(extracted)])
        legacy = extracted / "usr/src/cedarview-drm-20120717/staging/cdv/pvr"
        reference = compile_public(args.cc, args.objcopy, legacy, REFERENCE_DEFINES, output, "intel-i386", original=True)
        defines = public_defines(cmd.read_text())
        compat = compile_public(args.cc, args.objcopy, core, defines, output, "compat-i386")
        native = read_section(args.objcopy, obj, output, "native-amd64")
        assessment = compare(reference, compat, native)
        report = {"package": PACKAGE, "reference_defines": REFERENCE_DEFINES,
                  "compat_public_defines": defines, "reference_i386": reference,
                  "compat_i386": compat, "actual_kernel_amd64": native, "assessment": assessment,
                  "scope": "Selected initializer/transport/control layouts and build-option mask; not complete firmware or rendering compatibility",
                  "objects_executed": False, "modules_loaded": False, "libraries_executed": False}
        (output / "init-abi-report.json").write_text(json.dumps(report, indent=2) + "\n")
        print("Initializer ABI report:", output / "init-abi-report.json")
        print("i386 request:", reference["init2_request_bytes"], "bytes; native kernel request:", native["init2_request_bytes"], "bytes")
        if not assessment["selected_i386_wire_supported"] or not assessment["selected_firmware_control_layouts_match"] or not assessment["masked_build_options_match"]:
            print("FAIL: selected ABI measurements differ; inspect report")
            return 1
        print("PASS: selected Intel i386 layouts, translated payload forms and firmware control sizes; native structures require explicit translation")
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
