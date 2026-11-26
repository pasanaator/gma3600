# SPDX-License-Identifier: GPL-2.0-only
import copy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location("boot_audit", ROOT / "tools/sgx545_boot_audit.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class BootAuditTests(unittest.TestCase):
    def test_capture_addresses_coverage_and_readiness_are_distinct(self):
        report = json.loads((ROOT / "research/sgx545/d2500hn-boot-inputs.json").read_text())
        with tempfile.TemporaryDirectory() as directory:
            headers = [Path(directory) / name for name in ("intel.h", "compat.h")]
            for path in headers:
                path.write_text("#define EUR_CR_EVENT_HOST_ENABLE 0x0130\n#define EUR_CR_EVENT_HOST_ENABLE_MASK 0xffffffffU\n")
            result = module.audit(report, headers)
            self.assertTrue(result["capture_evidence_consistent"])
            self.assertFalse(result["execution_ready"])
            self.assertFalse(result["hardware_execution_enabled"])
            self.assertEqual(result["register_coverage"]["writes"], 49)
            self.assertEqual(result["register_coverage"]["names_in_all_headers"], 1)
            self.assertEqual(result["polling_changes"], [{"offset": 0x130, "original": 0x4000, "planned": 0}])
            changed = copy.deepcopy(report)
            changed["contents"]["buffers"][10]["address"] += 0x100000
            self.assertFalse(module.audit(changed, headers)["capture_evidence_consistent"])

    def test_tampering_or_stale_resource_counters_does_not_pass(self):
        report = json.loads((ROOT / "research/sgx545/d2500hn-boot-inputs.json").read_text())
        with tempfile.TemporaryDirectory() as directory:
            headers = [Path(directory) / name for name in ("intel.h", "compat.h")]
            for path in headers:
                path.write_text("")
            changed = copy.deepcopy(report)
            changed["startup_inputs"]["control"]["clock0"] ^= 1
            self.assertFalse(module.audit(changed, headers)["capture_evidence_consistent"])
            changed = copy.deepcopy(report)
            changed["after"]["bm_pages"] = "1"
            self.assertFalse(module.audit(changed, headers)["capture_evidence_consistent"])
            changed = copy.deepcopy(report)
            changed["startup_inputs"]["scripts"]["part2"]["writes"][0]["offset"] = 0x4000
            with self.assertRaises(ValueError):
                module.audit(changed, headers)


if __name__ == "__main__":
    unittest.main()
