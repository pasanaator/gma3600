#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the opt-in SGX host/probe and, optionally, services preparation.

Never installs/loads modules, downloads source, or runs proprietary libraries.
All builds occur in a new output directory, leaving input checkouts untouched.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

PIN = "4000c3fb6577ecad4800e4fa2695777643bfaf86"
SUBPATH = "drivers/gpu/drm/pvrsgx/1.17.4948957/eurasia_km"
ROOT = Path(__file__).resolve().parents[1]


def git(source, *args):
    return subprocess.check_output(["git", "-C", str(source), *args])


def copy_inputs(source, destination):
    destination.mkdir()
    for item in source.iterdir():
        if item.name == "Makefile" or item.suffix in (".c", ".h"):
            shutil.copy2(item, destination / item.name)


def run_logged(command, directory, log, report):
    print(f"Building {log.stem}; log: {log}", flush=True)
    entry = {"argv": [str(arg) for arg in command], "cwd": str(directory),
             "log": str(log)}
    report["commands"].append(entry)
    with log.open("w") as stream:
        result = subprocess.run(entry["argv"], cwd=directory,
                                stdout=stream, stderr=subprocess.STDOUT)
    entry["exit_code"] = result.returncode
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}); see {log}")


def record_module(report, name, path):
    with path.open("rb") as stream:
        header = stream.read(20)
    if (header[:6] != b"\x7fELF\x02\x01" or
            int.from_bytes(header[18:20], "little") != 62):
        raise RuntimeError(f"Expected an ELF64 x86-64 module: {path}")
    report["modules"][name] = str(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kdir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True,
                        help="new directory; existing paths are rejected")
    parser.add_argument("--source", type=Path,
                        help="optional pinned openpvrsgx checkout for services preparation")
    parser.add_argument("--sgx-rev", choices=("10131", "10141"), default="10141",
                        help="compile profile only; hardware revision remains unverified")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    kdir = args.kdir.resolve()
    output = args.output.absolute()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if output.exists() or output.is_symlink():
        parser.error("--output must be a new directory")
    if not (kdir / "Makefile").is_file() or not (kdir / ".config").is_file():
        parser.error("--kdir must contain configured kernel headers")
    config = (kdir / ".config").read_text()
    for setting in ("CONFIG_X86_64=y", "CONFIG_COMPAT=y", "CONFIG_AUXILIARY_BUS=y"):
        if setting not in config.splitlines():
            parser.error(f"target requires {setting}")
    source = args.source.resolve() if args.source else None
    if source:
        if git(source, "rev-parse", "HEAD").decode().strip() != PIN:
            parser.error(f"--source must be at {PIN}")
        if subprocess.run(["git", "-C", str(source), "diff", "--no-ext-diff",
                           "--no-textconv", "--quiet", "HEAD", "--", SUBPATH]).returncode:
            parser.error("pinned source subtree has tracked changes")
    output.mkdir(parents=True)
    report = {
        "status": "building", "target": "Debian 13 amd64",
        "kernel_headers": str(kdir), "sgx_runtime_enabled": False,
        "firmware_boot_compiled": bool(source), "firmware_boot_default": False,
        "render_submission_enabled": False, "hardware_executed_by_builder": False,
        "source_pin": PIN if source else None,
        "unverified_sgx_compile_profile": args.sgx_rev if source else None,
        "commands": [], "modules": {},
    }
    make = ["make", "-C", str(kdir)]
    try:
        host = output / "host"
        copy_inputs(ROOT / "kernel/gma3600", host)
        run_logged(make + [f"M={host}", "SGX_HOST=1", f"-j{args.jobs}", "modules"],
                   output, output / "host.log", report)
        record_module(report, "host", host / "gma3600.ko")
        probe = output / "probe"
        copy_inputs(ROOT / "experimental/sgx545/probe", probe)
        shutil.copy2(ROOT / "kernel/gma3600/gma3600_sgx.h", probe)
        run_logged(make + [f"M={probe}", f"-j{args.jobs}", "modules"],
                   output, output / "probe.log", report)
        record_module(report, "probe", probe / "gma3600_sgx_probe.ko")
        if source:
            core = output / "core"
            core.mkdir()
            paths = git(source, "ls-files", "-z", "--", SUBPATH).split(b"\0")
            for raw in filter(None, paths):
                relative = Path(os.fsdecode(raw)).relative_to(SUBPATH)
                destination = core / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source / SUBPATH / relative, destination)
            for patch in sorted((ROOT / "experimental/sgx545/patches").glob("*.patch")):
                run_logged(["patch", "--batch", "--fuzz=0", "-p1", "-i", str(patch)],
                           core, output / (patch.stem + ".log"), report)
            for item in (ROOT / "experimental/sgx545/core").iterdir():
                if item.suffix in (".c", ".h", ".inc"):
                    shutil.copy2(item, core)
            shutil.copy2(ROOT / "kernel/gma3600/gma3600_sgx.h", core)
            for item in (ROOT / "experimental/sgx545/abi").iterdir():
                shutil.copy2(item, core)
            with (core / "Makefile").open("a") as stream:
                stream.write("\n$(TARGET) += sgx545_abi.o sgx545_protocol.o gma3600_sgx_abi_manifest.o gma3600_sgx_init.o gma3600_sgx_boot.o gma3600_sgx_live.o\n")
            run_logged([sys.executable, str(ROOT / "tests/sgx545_source_test.py"),
                        str(core), str(output / "source-tests")],
                       output, output / "source-tests.log", report)
            run_logged(make + [f"M={core}", "CONFIG_SGX_DRM=y", "CONFIG_SGX_CEDARVIEW=m",
                       "CONFIG_SGX_HW_PERF=y", "CONFIG_SGX_HW_RECOVERY=y",
                       "CONFIG_SGX_PB_PER_CONTEXT=y", "CONFIG_PVRSGX_1_7_862890_ABI=y",
                       "CONFIG_SGX_IOCTL_ALIAS=18", "SOC_VENDOR=intel",
                       "SOC_FAMILY=cedarview_linux", "SOC=gma3600", "SGX=sgx545",
                       "SGX_FLAG=SGX545", f"SGX_REV={args.sgx_rev}", "TARGET_TYPE=m",
                       "AM_VERSION=0", f"-j{args.jobs}", "modules"],
                       output, output / "core.log", report)
            record_module(report, "services_prepare_only", core / "gma3600_sgx_services.ko")
            client = output / "sgx545-compat-probe"
            run_logged(["bash", str(ROOT / "tools/sgx545_compat_probe_build.sh"), str(client)],
                       output, output / "compat-client.log", report)
            header = client.read_bytes()[:20]
            if header[:6] != b"\x7fELF\x01\x01" or int.from_bytes(header[18:20], "little") != 3:
                raise RuntimeError("Expected an ELF32 i386 diagnostic client")
            report["compat_diagnostic_client"] = str(client)
        report["status"] = "passed"
    except (OSError, subprocess.CalledProcessError, RuntimeError) as error:
        report["status"] = "failed"
        report["error"] = str(error)
        print(error, file=sys.stderr)
    finally:
        (output / "build-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Build {report['status']}: {output / 'build-report.json'}")
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
