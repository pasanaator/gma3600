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
    if manifest.get("transport_version") != "0.6.0":
        raise ValueError("Rebuild the initializer bundle with the current helper; transport 0.6.0 is required")
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
    if host != "1.0.4-sgx-host2" or child != "0.14.0":
        raise ValueError("Capture currently requires host2 and tested services 0.14.0")
    status = services_status(sysroot)
    required = {"state": "prepared", "devices": "1", "gpu_execution": "disabled", "bridge": "enabled",
                "pages_live": "4", "blocked_hw_accesses": "0", "init_ran": "0"}
    required.update({key: "0" for key in ("files", "connections", "bridge_errors", "heap_handles", "init_running",
                    "buffers", "buffer_pages", "buffer_maps", "buffer_errors", "map_leases", "device_allocations", "device_pages",
                    "device_errors", "stages", "staged_buffers", "staged_pages", "stage_errors", "native_bindings", "native_errors")})
    if any(status.get(key) != value for key, value in required.items()):
        raise ValueError("Services must be fresh, idle and prepare-only; unload/reload the child after any initializer test")
    return {"parent_version": host, "services_version": child, "status": status}


def clean_status(status):
    expected = {"state": "prepared", "gpu_execution": "disabled", "blocked_hw_accesses": "0", "pages_live": "4"}
    expected.update({key: "0" for key in ("files", "connections", "bridge_errors", "heap_handles", "init_running",
                    "buffers", "buffer_pages", "buffer_maps", "buffer_errors", "map_leases", "device_allocations", "device_pages",
                    "device_errors", "stages", "staged_buffers", "staged_pages", "stage_errors", "native_bindings", "native_errors")})
    return all(status.get(key) == value for key, value in expected.items())


def handoff_status(stderr):
    matches = re.findall(r"^HANDOFF validated=([01]) staged=([01]) queried=([01]) aborted=([01]) "
                         r"buffers=(\d+) pages=(\d+) errno=(\d+) gpu_execution=disabled$", stderr, re.M)
    if not matches:
        return None
    values = list(map(int, matches[-1]))
    status = dict(zip(("validated", "staged", "queried", "aborted", "buffers", "pages", "errno"), values))
    status["cpu_cycle_passed"] = values[:4] == [1, 1, 1, 1] and values[6] == 0 and 9 <= values[4] <= 27 and values[5] >= values[4]
    return status


def first_failed_request(stderr):
    """Describe the first compatibility failure, excluding later cleanup calls."""
    current = None
    for line in stderr.splitlines():
        match = re.fullmatch(r"BRIDGE command=(\d+) in=(\d+) out=(\d+)", line)
        if match:
            current = dict(zip(("command", "input_bytes", "output_bytes"), map(int, match.groups())))
            continue
        if current is None:
            continue
        match = re.fullmatch(r"ALLOC heap=([0-9a-f]+) flags=([0-9a-f]+) bytes=(\d+) alignment=(\d+)", line)
        if match and current["command"] == 6:
            current.update(heap_handle=int(match[1], 16), flags=int(match[2], 16),
                           bytes=int(match[3]), alignment=int(match[4]))
        match = re.fullmatch(r"RESULT command=(\d+) (errno|payload_error)=(\d+)", line)
        if match and int(match[1]) == current["command"] and int(match[3]):
            return {**current, match[2]: int(match[3])}
        if line == "BLOCK bridge command=" + str(current["command"]):
            return {**current, "unsupported": True}
        if current["command"] == 99 and re.fullmatch(r"NATIVE errno=\d+", line):
            return {**current, "errno": int(line.split("=")[1]), "phase": "native_cpu_cycle"}
        if current["command"] == 99 and line.startswith("RUNTIME "):
            runtime = native_runtime_status(line, None)
            if runtime.get("errno"):
                return {**current, "errno": runtime["errno"], "phase": "native_runtime"}
        if current["command"] == 99 and line.startswith("HANDOFF "):
            handoff = handoff_status(line)
            if handoff and handoff["errno"]:
                return {**current, "errno": handoff["errno"], "phase": "cpu_handoff"}
    return None


def content_status(stderr, handoff):
    rows = []
    for match in re.finditer(r"^CONTENT index=(\d+) handle=([0-9a-f]+) bytes=(\d+) address=([0-9a-f]+) memory_flags=([0-9a-f]+) "
                             r"baseline=([0-9a-f]+) current=([0-9a-f]+) nonzero=(\d+) padding_nonzero=(\d+) changed=(\d+) mapped=([01])$", stderr, re.M):
        values = [int(value, 16 if i in (1, 3, 4, 5, 6) else 10) for i, value in enumerate(match.groups())]
        rows.append(dict(zip(("index", "handle", "bytes", "address", "memory_flags", "baseline_hash", "current_hash",
                              "nonzero_bytes", "padding_nonzero", "changed", "mapped"), values)))
    summaries = re.findall(r"^CONTENT_SUMMARY checked=(\d+) buffers=(\d+) bytes=(\d+) nonzero_bytes=(\d+) populated=(\d+) changed=(\d+) errno=(\d+) mapped=(\d+)$", stderr, re.M)
    if not summaries:
        return {"buffers": rows, "summary": None, "verified": False}
    summary = dict(zip(("checked", "buffers", "bytes", "nonzero_bytes", "populated", "changed", "errno", "mapped"), map(int, summaries[-1])))
    verified = bool(handoff and handoff["cpu_cycle_passed"] and not summary["errno"] and not summary["changed"] and
                    summary["checked"] == summary["buffers"] == handoff["buffers"] == len(rows) and
                    [r["index"] for r in rows] == list(range(len(rows))) and
                    len({r["handle"] for r in rows}) == len(rows) and
                    sum(r["bytes"] for r in rows) == summary["bytes"] and
                    sum(r["nonzero_bytes"] for r in rows) == summary["nonzero_bytes"] and
                    sum(r["nonzero_bytes"] != 0 for r in rows) == summary["populated"] and
                    sum(r["mapped"] for r in rows) == summary["mapped"] and
                    all(not r["changed"] and r["baseline_hash"] == r["current_hash"] and
                        0 <= r["nonzero_bytes"] <= r["bytes"] for r in rows))
    return {"buffers": rows, "summary": summary, "verified": verified, "checksum": "FNV-1a-32; change detection only"}


def native_cycle_status(stderr, handoff):
    matches = re.findall(r"^NATIVE bound=([01]) restored=([01]) ccb_bytes=(\d+) slots=(\d+) read=(\d+) write=(\d+) "
                         r"roundtrip=([01]) buffers=(\d+) pages=(\d+) gpu_execution=disabled$", stderr, re.M)
    status = dict(zip(("bound", "restored", "ccb_bytes", "slots", "read_offset", "write_offset", "roundtrip", "buffers", "pages"),
                      map(int, matches[-1]))) if matches else {}
    status["verified"] = bool(len(matches) == 1 and handoff and handoff["cpu_cycle_passed"] and
                              status["bound"] == status["restored"] == status["roundtrip"] == 1 and
                              status["ccb_bytes"] == 8192 and status["slots"] == 256 and
                              0 <= status["read_offset"] == status["write_offset"] < status["slots"] and
                              status["buffers"] == handoff["buffers"] and status["pages"] == handoff["pages"] and
                              not re.search(r"^NATIVE errno=", stderr, re.M))
    return status


def native_runtime_status(stderr, handoff):
    matches = re.findall(r"^RUNTIME bound=([01]) queried=([01]) unbound=([01]) buffers=(\d+) pages=(\d+) errno=(\d+) gpu_execution=disabled$", stderr, re.M)
    status = dict(zip(("bound", "queried", "unbound", "buffers", "pages", "errno"), map(int, matches[-1]))) if matches else {}
    status["verified"] = bool(len(matches) == 1 and handoff and handoff["cpu_cycle_passed"] and
                              status["bound"] == status["queried"] == status["unbound"] == 1 and not status["errno"] and
                              status["buffers"] == handoff["buffers"] and status["pages"] == handoff["pages"])
    return status


def mapping_release_status(stderr, contents):
    releases = []
    for match in re.finditer(r"^MAP_RELEASE handle=([0-9a-f]+) unmap=([01]) address=([0-9a-f]+) bytes=(\d+)$", stderr, re.M):
        releases.append(dict(zip(("handle", "unmap", "address", "bytes"),
                                 (int(value, 16 if i in (0, 2) else 10) for i, value in enumerate(match.groups())))))
    rows = contents["buffers"]
    tokens = {row["handle"] for row in rows}
    verified = bool(contents["verified"] and contents["summary"]["mapped"] == 0 and len(releases) == len(rows) and
                    {r["handle"] for r in releases} == tokens and
                    all(r["unmap"] == 1 and r["address"] and r["bytes"] > 0 and
                        not (r["address"] & 4095) and not (r["bytes"] & 4095) for r in releases))
    if verified:
        sizes = {row["handle"]: (row["bytes"] + 4095) & ~4095 for row in rows}
        verified = all(release["bytes"] == sizes[release["handle"]] for release in releases)
    return {"releases": releases, "all_staged_cpu_mappings_released": verified}


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
        handoff = handoff_status(stderr)
        contents = content_status(stderr, handoff)
        mappings = mapping_release_status(stderr, contents)
        native = native_cycle_status(stderr, handoff)
        runtime = native_runtime_status(stderr, handoff)
        if outcome != "inspect_report" and handoff and handoff["cpu_cycle_passed"]:
            outcome = "native_runtime_verified_with_clean_resources" if mappings["all_staged_cpu_mappings_released"] and native["verified"] and runtime["verified"] else "inspect_report"
        if contents["summary"] and not contents["verified"]:
            outcome = "inspect_report"
        report = {"schema": 1, "outcome": outcome, "initializer_process_exit": code, "before": before,
                  "transport_version": manifest["transport_version"],
                  "handoff": handoff,
                  "contents": contents,
                  "mapping_releases": mappings, "native_cycle": native, "native_runtime": runtime,
                  "first_failed_request": first_failed_request(stderr),
                  "after": after, "resources_clean": cleanup_ok, "stdout": stdout, "stderr": stderr,
                  "bridge_commands": [int(n) for n in re.findall(r"^BRIDGE command=(\d+)", stderr, re.M)],
                  "bundle_artifact_sha256": manifest["artifact_sha256"],
                  "scope": "Unmodified Intel initializer and userspace services; private libdrm transport; GPU execution disabled"}
        report_stream.seek(0)
        report_stream.write(json.dumps(report, indent=2) + "\n")
        report_stream.truncate()
        report_stream.close()
        print("Initializer diagnostic report:", args.report)
        if outcome == "native_runtime_verified_with_clean_resources":
            print("Retained native binding, query, content checks and unbind verified:", runtime)
            print("Native CPU bind, CCB write/readback/restore and unbind verified:", native)
            print("CPU buffer contents, staging, PTE query and abort verified:", contents["summary"])
            print("Legacy CPU mappings released; staged RAM and GPU PTEs remained valid.")
            print("The Intel initializer is deliberately returned a failure after that diagnostic cycle.")
        elif "bound" in runtime and not runtime["verified"]:
            print("Retained native binding cycle incomplete:", runtime)
            if report["first_failed_request"]:
                print("First compatibility failure:", json.dumps(report["first_failed_request"], sort_keys=True))
        elif contents["verified"] and not mappings["all_staged_cpu_mappings_released"]:
            print("CPU mapping release audit incomplete: released records=", len(mappings["releases"]),
                  "staged buffers=", len(contents["buffers"]), "remaining mapped=", contents["summary"]["mapped"])
        elif contents["summary"]:
            print("Buffer content audit incomplete or changed:", contents["summary"])
        elif report["first_failed_request"]:
            print("First compatibility failure:", json.dumps(report["first_failed_request"], sort_keys=True))
        print("GPU execution remains disabled.")
        return 0 if outcome != "inspect_report" else 1
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    sys.exit(main())
