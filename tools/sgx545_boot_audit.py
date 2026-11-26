#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Read-only audit of captured SGX545 boot inputs; never executes a boot."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

from sgx545_initializer_run import clean_status


def register_names(path):
    names = {}
    source = path.read_text()
    for name, value in re.findall(r"^#define\s+(EUR_CR_\w+)\s+(0x[0-9a-fA-F]+)[UuLl]*\s*$", source, re.M):
        if name.endswith(("_MASK", "_SHIFT", "_SIGNED")):
            continue
        names.setdefault(int(value, 16), set()).add(name)
    return names


def audit(report, headers):
    startup = report["startup_inputs"]
    payload = {key: startup[key] for key in ("scripts", "control", "kick", "data")}
    digest = hashlib.sha256(json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    buffers = report["contents"]["buffers"]
    if len(buffers) != 15 or [b["index"] for b in buffers] != list(range(15)):
        raise ValueError("Expected exactly 15 original buffers in capture order")
    for b in buffers:
        for field in ("handle", "address", "bytes", "memory_flags"):
            if type(b[field]) is not int or not 0 <= b[field] <= 0xffffffff:
                raise ValueError("Invalid buffer word: " + field)
        if not b["address"] or b["address"] & 4095 or not 0 < b["bytes"] <= 1048576:
            raise ValueError("Invalid original buffer address/size")
        if b["address"] + ((b["bytes"] + 4095) & ~4095) > 0x100000000:
            raise ValueError("Original buffer wraps the GPU address space")
    definitions = [register_names(path) for path in headers]
    rows, offsets, shared, absent = [], set(), set(), set()
    script_valid = True
    part2 = {}
    for phase, limit in (("part1", 64), ("part2", 64), ("deinit", 16)):
        script = startup["scripts"][phase]
        writes = script["writes"]
        script_valid &= script["terminated_and_complete"] is True and len(writes) < limit
        for index, w in enumerate(writes):
            if any(type(w[field]) is not int or not 0 <= w[field] <= 0xffffffff for field in ("index", "offset", "value")):
                raise ValueError("Invalid script word")
            if w["index"] != index or w["offset"] >= 0x4000 or w["offset"] & 3:
                raise ValueError("Invalid script offset/order")
            offset = w["offset"]
            offsets.add(offset)
            sets = [d.get(offset, set()) for d in definitions]
            common = set.intersection(*sets) if sets else set()
            if common:
                shared.add(offset)
            if not any(sets):
                absent.add(offset)
            rows.append({"phase": phase, **w, "names_in_all_headers": sorted(common),
                         "names_by_header": [sorted(s) for s in sets],
                         "value_semantics_verified": False})
            if phase == "part2":
                if offset in part2:
                    raise ValueError("Repeated part2 offset is outside the captured recipe")
                part2[offset] = w["value"]
    checks = [
        {"name": "kernel_code_base_captured_encoding", "offset": 0xa48,
         "expected": (buffers[9]["address"] >> 7) | 0x06000000,
         "note": "Observed Intel encoding; no new assertion of undocumented address semantics"},
        {"name": "event_kicker_original_allocation", "offset": 0xac4,
         "expected": buffers[2]["address"], "note": "Original allocation, not diagnostic alias"},
        {"name": "boot_pds_original_allocation", "offset": 0xa68,
         "expected": buffers[10]["address"], "note": "Observed pointer relationship; register unnamed in public headers"},
    ]
    for check in checks:
        check["supplied"] = part2.get(check["offset"])
        check["matches_original"] = check["supplied"] == check["expected"]
    consistent = (startup["complete"] is True and script_valid and digest == startup["sha256"] and
                  report["resources_clean"] is True and clean_status(report["after"]) and
                  report["contents"]["verified"] is True and report["native_ownership"]["verified"] is True and
                  all(c["matches_original"] for c in checks))
    return {"schema": 1, "capture_evidence_consistent": bool(consistent), "startup_input_sha256": digest,
            "hardware_execution_enabled": False, "execution_ready": False,
            "headers": [{"path": str(p), "sha256": hashlib.sha256(p.read_bytes()).hexdigest()} for p in headers],
            "register_coverage": {"writes": len(rows), "unique_offsets": len(offsets),
                                  "names_in_all_headers": len(shared), "absent_from_all_headers": sorted(absent)},
            "address_checks": checks, "scripts": rows,
            "polling_changes": [{"offset": offset, "original": value, "planned": 0}
                                for offset, value in part2.items() if offset in (0x130, 0x110) and value],
            "limits": ["Header names do not prove register value semantics or safe sequencing",
                       "Observed-only recipe must stay restricted to the pinned initializer/profile",
                       "No hardware power, DMA, firmware completion or ABI compatibility validated by this audit",
                       "Boot controller is not connected to a live bridge/MMIO provider; parent ownership integration remains"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--headers", type=Path, nargs=2, required=True,
                        help="original Intel and pinned compatibility sgx545defs.h")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        if args.report.stat().st_size > 8 * 1024 * 1024:
            raise ValueError("Capture exceeds the 8 MiB input limit")
        result = audit(json.loads(args.report.read_text()), args.headers)
        text = json.dumps(result, indent=2) + "\n"
        if args.output:
            with args.output.open("x") as stream:
                stream.write(text)
        else:
            sys.stdout.write(text)
        return 0 if result["capture_evidence_consistent"] else 1
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    sys.exit(main())
