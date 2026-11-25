#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the project-owned loader selftest, or capture Intel initializer requests.

Real capture is restricted to the user's supported Intel board with fresh,
prepare-only services. This helper never loads modules or enables execution.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys

ARTIFACTS = {"sgx545-initializer", "lib/libdrm.so.2", "lib/libsrv_init.so", "lib/libsrv_um.so",
             "lib/libsgx545_initializer_fixture.so"}
ARTIFACTS.update("sysroot/lib/i386-linux-gnu/" + name for name in
                 ("ld-2.15.so", "libc.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1"))


def verify_bundle(bundle):
    manifest = json.loads((bundle / "bundle.json").read_text())
    hashes = manifest["artifact_sha256"]
    if manifest.get("transport_version") != "0.1.4":
        raise ValueError("Rebuild the initializer bundle with the current helper; transport 0.1.4 is required")
    if manifest.get("schema") != 1 or set(hashes) != ARTIFACTS:
        raise ValueError("Unknown/incomplete initializer bundle")
    for name, expected in hashes.items():
        if hashlib.sha256((bundle / name).read_bytes()).hexdigest() != expected:
            raise ValueError("Bundle artifact changed: " + name)
    return manifest


def services_status(sysroot=Path("/sys")):
    files = list((sysroot / "bus/auxiliary/devices").glob("gma3600.sgx.*/services"))
    if len(files) != 1:
        raise ValueError("Expected one loaded SGX services child")
    text = files[0].read_text().strip()
    return {item.split("=", 1)[0]: item.split("=", 1)[1] for item in text.split()}


def check_capture_target(sysroot=Path("/sys"), architecture=None, uid=None):
    if (platform.machine() if architecture is None else architecture) != "x86_64":
        raise ValueError("Real capture requires Debian amd64 on the Intel Atom")
    if (os.geteuid() if uid is None else uid) != 0:
        raise ValueError("Capture needs sudo for the existing privileged initializer bridge")
    pci = sysroot / "bus/pci/devices/0000:00:02.0"
    if (pci / "vendor").read_text().strip() != "0x8086" or (pci / "device").read_text().strip() != "0x0be1":
        raise ValueError("Real initializer execution is restricted to the verified Intel GMA3600")
    if (pci / "driver").resolve().name != "gma3600":
        raise ValueError("PCI display parent must be gma3600")
    host = (sysroot / "module/gma3600/version").read_text().strip()
    child = (sysroot / "module/gma3600_sgx_services/version").read_text().strip()
    if host != "1.0.4-sgx-host2" or child != "0.9.4":
        raise ValueError("Capture currently requires host2 and tested services 0.9.4")
    status = services_status(sysroot)
    required = {"state": "prepared", "devices": "1", "gpu_execution": "disabled", "bridge": "enabled",
                "pages_live": "4", "blocked_hw_accesses": "0", "init_ran": "0"}
    required.update({key: "0" for key in ("files", "connections", "bridge_errors", "heap_handles", "init_running",
                    "buffers", "buffer_pages", "buffer_maps", "buffer_errors", "device_allocations", "device_pages",
                    "device_errors", "stages", "staged_buffers", "staged_pages", "stage_errors")})
    if any(status.get(key) != value for key, value in required.items()):
        raise ValueError("Services must be fresh, idle and prepare-only; unload/reload the child after any initializer test")
    return {"parent_version": host, "services_version": child, "status": status}


def clean_status(status):
    expected = {"state": "prepared", "gpu_execution": "disabled", "blocked_hw_accesses": "0", "pages_live": "4"}
    expected.update({key: "0" for key in ("files", "connections", "bridge_errors", "heap_handles", "init_running",
                    "buffers", "buffer_pages", "buffer_maps", "buffer_errors", "device_allocations", "device_pages",
                    "device_errors", "stages", "staged_buffers", "staged_pages", "stage_errors")})
    return all(status.get(key) == value for key, value in expected.items())


def command(bundle, selftest=False, qemu=None):
    loader = bundle / "sysroot/lib/i386-linux-gnu/ld-2.15.so"
    libraries = str(bundle / "lib") + ":" + str(loader.parent)
    args = [str(loader), "--library-path", libraries, str(bundle / "sgx545-initializer")]
    if selftest:
        args.append("--selftest")
    return ([str(qemu)] if qemu else []) + args


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--selftest", action="store_true", help="only project-owned fixture; no driver/hardware needed")
    mode.add_argument("--capture", action="store_true", help="invoke the unmodified Intel initializer on the verified Atom")
    parser.add_argument("--report", type=Path, help="new JSON file, required for capture")
    parser.add_argument("--qemu", type=Path, help="i386 emulator for the open-source selftest only")
    args = parser.parse_args()
    try:
        bundle = args.bundle.resolve(strict=True)
        manifest = verify_bundle(bundle)
        before = None
        report_stream = None
        if args.capture:
            if args.qemu:
                raise ValueError("Real capture must run natively on the Intel hardware")
            if not args.report or args.report.exists():
                raise ValueError("Capture needs --report pointing to a new JSON file")
            before = check_capture_target()
            # Establish a writable, exclusive report before calling proprietary
            # code. Even an interrupted helper leaves a pending-capture record.
            report_stream = args.report.open("x")
            report_stream.write(json.dumps({"schema": 1, "outcome": "capture_pending", "before": before}) + "\n")
            report_stream.flush()
        # Ignore external loader overrides and desktop configuration. These
        # private libraries are never added to the system linker configuration.
        environment = {"PATH": "/usr/bin:/bin", "HOME": str(bundle / "empty-home"), "LANG": "C", "LC_ALL": "C"}
        try:
            result = subprocess.run(command(bundle, args.selftest, args.qemu), cwd=bundle, env=environment,
                                    capture_output=True, text=True, timeout=25, errors="replace")
            code, stdout, stderr = result.returncode, result.stdout, result.stderr
        except subprocess.TimeoutExpired as error:
            code = None
            stdout = (error.stdout or b"").decode(errors="replace")
            stderr = (error.stderr or b"").decode(errors="replace")
        print(stdout, end="")
        print(stderr, end="", file=sys.stderr)
        if args.selftest:
            if code != 0:
                raise ValueError("Open-source loader fixture failed (no proprietary initializer was called)")
            print("PASS: private i386 loader, project-owned fixture and limited libdrm transport")
            return 0
        try:
            after = services_status()
            cleanup_ok = clean_status(after)
        except (OSError, ValueError) as error:
            after, cleanup_ok = {"status_error": str(error)}, False
        outcome = "initializer_returned_failure_with_clean_resources" if code == 3 and cleanup_ok else "inspect_report"
        report = {"schema": 1, "outcome": outcome, "initializer_process_exit": code, "before": before,
                  "transport_version": manifest["transport_version"],
                  "after": after, "resources_clean": cleanup_ok, "stdout": stdout, "stderr": stderr,
                  "bridge_commands": [int(n) for n in re.findall(r"^BRIDGE command=(\d+)", stderr, re.M)],
                  "bundle_artifact_sha256": manifest["artifact_sha256"],
                  "scope": "Unmodified Intel initializer and userspace services; private libdrm transport; GPU execution disabled"}
        report_stream.seek(0)
        report_stream.write(json.dumps(report, indent=2) + "\n")
        report_stream.truncate()
        report_stream.close()
        print("Initializer diagnostic report:", args.report)
        print("This is a compatibility trace, not a successful GPU initialization.")
        return 0 if outcome != "inspect_report" else 1
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    sys.exit(main())
