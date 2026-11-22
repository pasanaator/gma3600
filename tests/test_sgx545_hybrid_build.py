# SPDX-License-Identifier: GPL-2.0-only
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BUILDER = Path(__file__).resolve().parents[1] / "tools/sgx545_hybrid_build.py"


class HybridBuildTests(unittest.TestCase):
    def invoke(self, kdir, output, env=None):
        return subprocess.run([sys.executable, str(BUILDER), "--kdir", str(kdir),
                               "--output", str(output)], env=env, capture_output=True,
                              text=True)

    def test_existing_output_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "output"
            output.mkdir()
            sentinel = output / "keep"
            sentinel.write_text("untouched")
            result = self.invoke(root / "missing-headers", output)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(sentinel.read_text(), "untouched")

    def test_missing_compat_fails_before_build_or_output_creation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "Makefile").write_text("$(error must not run make)\n")
            (root / ".config").write_text("CONFIG_X86_64=y\nCONFIG_AUXILIARY_BUS=y\n")
            result = self.invoke(root, root / "output")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("CONFIG_COMPAT=y", result.stderr)
            self.assertFalse((root / "output").exists())

    def test_make_failure_is_preserved_in_report(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "Makefile").touch()
            (root / ".config").write_text(
                "CONFIG_X86_64=y\nCONFIG_COMPAT=y\nCONFIG_AUXILIARY_BUS=y\n")
            make = root / "make"
            make.write_text("#!/bin/sh\nexit 7\n")
            make.chmod(0o755)
            env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ["PATH"])
            output = root / "output"
            result = self.invoke(root, output, env)
            self.assertEqual(result.returncode, 1)
            report = json.loads((output / "build-report.json").read_text())
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["commands"][0]["exit_code"], 7)
            self.assertEqual(report["modules"], {})
            self.assertFalse(report["sgx_runtime_enabled"])

    def test_successful_make_without_module_does_not_report_success(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "Makefile").touch()
            (root / ".config").write_text(
                "CONFIG_X86_64=y\nCONFIG_COMPAT=y\nCONFIG_AUXILIARY_BUS=y\n")
            make = root / "make"
            make.write_text("#!/bin/sh\nexit 0\n")
            make.chmod(0o755)
            env = dict(os.environ, PATH=str(root) + os.pathsep + os.environ["PATH"])
            output = root / "output"
            result = self.invoke(root, output, env)
            self.assertEqual(result.returncode, 1)
            report = json.loads((output / "build-report.json").read_text())
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["modules"], {})


if __name__ == "__main__":
    unittest.main()
