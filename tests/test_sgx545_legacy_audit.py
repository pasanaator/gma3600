# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / "tools"
spec = importlib.util.spec_from_file_location("sgx545_legacy_audit", TOOLS / "sgx545_legacy_audit.py")
audit = importlib.util.module_from_spec(spec)
sys.path.insert(0, str(TOOLS))
try:
    spec.loader.exec_module(audit)
finally:
    sys.path.pop(0)


class LegacyAuditTests(unittest.TestCase):
    def test_hash_and_length_reject_tampering_before_extraction(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "candidate.deb"
            path.write_bytes(b"verified fixture")
            metadata = {"size": path.stat().st_size,
                        "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
            self.assertEqual(audit.verify_package(path, metadata), metadata["sha256"])
            path.write_bytes(b"tampered fixture")
            with patch.object(audit, "command") as command:
                with self.assertRaisesRegex(ValueError, "refusing extraction"):
                    audit.audit_package(path, {"package": metadata})
                command.assert_not_called()
            path.write_bytes(b"short")
            with self.assertRaises(ValueError):
                audit.verify_package(path, metadata)

    def test_readelf_parsers_distinguish_imports_exports_and_versions(self):
        needed, soname = audit.parse_dynamic(
            " 0x1 (NEEDED) Shared library: [libc.so.6]\n"
            " 0xe (SONAME) Library soname: [libGLESv2.so]\n")
        self.assertEqual(needed, ["libc.so.6"])
        self.assertEqual(soname, "libGLESv2.so")
        exports = audit.parse_exports(
            " 1: 00000100 20 FUNC GLOBAL DEFAULT 9 eglCreatePbufferSurface\n"
            " 2: 00000000 0 FUNC GLOBAL DEFAULT UND glClear\n"
            " 3: 00000200 20 FUNC LOCAL DEFAULT 9 glReadPixels\n"
            " 4: 00000300 20 FUNC WEAK DEFAULT 9 glFinish@@TEST_1\n")
        self.assertEqual(exports, ["eglCreatePbufferSurface", "glFinish"])
        versions = audit.parse_versions(
            "Version definition section '.gnu.version_d' contains 1 entry:\n"
            " Name: OWN_VERSION\n"
            "Version needs section '.gnu.version_r' contains 1 entry:\n"
            " Version: 1 File: libc.so.6 Cnt: 2\n"
            " Name: GLIBC_2.0 Flags: none Version: 2\n"
            " Name: GLIBC_2.4 Flags: none Version: 3\n")
        self.assertEqual(versions, {"libc.so.6": ["GLIBC_2.0", "GLIBC_2.4"]})

    def test_dependency_closure_follows_aliases_and_cycles_without_soname(self):
        libraries = [
            {"path": "libEGL.so.1.7", "aliases": ["libEGL.so", "libEGL.so.1.7"],
             "needed": ["libIMGegl.so", "libc.so.6"]},
            {"path": "libIMGegl.so.1.7", "aliases": ["libIMGegl.so"],
             "needed": ["libEGL.so", "libsrv_um.so"]},
            {"path": "libsrv_um.so.1.7", "aliases": ["libsrv_um.so"],
             "needed": ["libdrm.so.2", "libc.so.6"]},
        ]
        result = audit.dependency_closure(libraries, ["libEGL.so"])
        self.assertEqual(result["external_sonames"], ["libc.so.6", "libdrm.so.2"])
        self.assertEqual(len(result["internal_libraries"]), 3)
        self.assertIn("dynamically", result["scope"])


if __name__ == "__main__":
    unittest.main()
