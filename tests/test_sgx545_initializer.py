# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import importlib.util
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


builder = load("sgx545_initializer_build")
runner = load("sgx545_initializer_run")


class InitializerTests(unittest.TestCase):
    def test_pinned_package_validation_rejects_tampering_and_length(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.deb"
            path.write_bytes(b"fixture")
            metadata = {"filename": path.name, "size": 7, "sha256": hashlib.sha256(b"fixture").hexdigest()}
            builder.verify(path, metadata)
            self.assertEqual(builder.obtain(path.parent, metadata, True), path)
            path.write_bytes(b"changed")
            with self.assertRaises(ValueError):
                builder.obtain(path.parent, metadata, True)
            path.unlink()
            with self.assertRaisesRegex(ValueError, "Missing cached"):
                builder.obtain(path.parent, metadata, True)

    def test_existing_output_preserved_before_download(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / "user-file").write_text("keep")
            with self.assertRaisesRegex(ValueError, "new directory"):
                builder.build(folder, folder / "no-cache", "no-compiler", True)
            self.assertEqual((folder / "user-file").read_text(), "keep")
            self.assertFalse((folder / "no-cache").exists())

    def test_runtime_and_intel_artifact_hashes_are_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            manifest = {"schema": 1, "transport_version": "0.1.3", "artifact_sha256": {}}
            for name in runner.ARTIFACTS:
                file = folder / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_bytes(b"test fixture")
                manifest["artifact_sha256"][name] = hashlib.sha256(file.read_bytes()).hexdigest()
            (folder / "bundle.json").write_text(json.dumps(manifest))
            runner.verify_bundle(folder)
            (folder / "bundle.json").write_text(json.dumps({**manifest, "transport_version": "0.1.0"}))
            with self.assertRaisesRegex(ValueError, "Rebuild.*0.1.3"):
                runner.verify_bundle(folder)
            (folder / "bundle.json").write_text(json.dumps(manifest))
            (folder / "sysroot/lib/i386-linux-gnu/libc.so.6").write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "artifact changed"):
                runner.verify_bundle(folder)

    def target(self, root):
        for file, value in {
            "bus/pci/devices/0000:00:02.0/vendor": "0x8086",
            "bus/pci/devices/0000:00:02.0/device": "0x0be1",
            "module/gma3600/version": "1.0.4-sgx-host2",
            "module/gma3600_sgx_services/version": "0.9.3",
        }.items():
            p = root / file
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(value + "\n")
        driver = root / "bus/pci/drivers/gma3600"
        driver.mkdir(parents=True)
        (root / "bus/pci/devices/0000:00:02.0/driver").symlink_to(driver)
        status = {"state": "prepared", "devices": "1", "gpu_execution": "disabled", "bridge": "enabled", "pages_live": "4"}
        status.update({key: "0" for key in ("blocked_hw_accesses", "init_ran", "files", "connections", "bridge_errors",
            "heap_handles", "init_running", "buffers", "buffer_pages", "buffer_maps", "buffer_errors",
            "device_allocations", "device_pages", "device_errors", "stages", "staged_buffers", "staged_pages", "stage_errors")})
        p = root / "bus/auxiliary/devices/gma3600.sgx.0/services"
        p.parent.mkdir(parents=True)
        return p, status

    def write_status(self, path, status):
        path.write_text(" ".join(key + "=" + value for key, value in status.items()) + "\n")

    def test_capture_requires_fresh_prepare_only_services(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path, status = self.target(root)
            self.write_status(path, status)
            self.assertEqual(runner.check_capture_target(root, "x86_64", 0)["services_version"], "0.9.3")
            for key, value in (("init_ran", "1"), ("gpu_execution", "enabled"), ("pages_live", "24"),
                               ("buffer_maps", "1"), ("stage_errors", "1"), ("blocked_hw_accesses", "1")):
                self.write_status(path, {**status, key: value})
                with self.assertRaisesRegex(ValueError, "fresh, idle"):
                    runner.check_capture_target(root, "x86_64", 0)
            self.write_status(path, status)
            self.assertTrue(runner.clean_status({**status, "init_ran": "1"}))
            self.assertFalse(runner.clean_status({**status, "stages": "1"}))
            self.assertFalse(runner.clean_status({}))

    def test_capture_rejects_wrong_architecture_privilege_hardware_or_version(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path, status = self.target(root)
            self.write_status(path, status)
            for architecture, uid in (("aarch64", 0), ("x86_64", 1000)):
                with self.assertRaises(ValueError):
                    runner.check_capture_target(root, architecture, uid)
            device = root / "bus/pci/devices/0000:00:02.0/device"
            device.write_text("0x1234\n")
            with self.assertRaisesRegex(ValueError, "verified Intel"):
                runner.check_capture_target(root, "x86_64", 0)
            device.write_text("0x0be1\n")
            (root / "module/gma3600_sgx_services/version").write_text("0.9.0\n")
            with self.assertRaisesRegex(ValueError, "services 0.9.3"):
                runner.check_capture_target(root, "x86_64", 0)

    def test_selftest_command_explicitly_selects_project_owned_fixture(self):
        bundle = Path("/bundle")
        selftest = runner.command(bundle, True, Path("/qemu-i386"))
        self.assertEqual(selftest[0], "/qemu-i386")
        self.assertEqual(selftest[-1], "--selftest")
        capture = runner.command(bundle)
        self.assertEqual(capture[0], "/bundle/sysroot/lib/i386-linux-gnu/ld-2.15.so")
        self.assertNotIn("--selftest", capture)
        self.assertEqual(capture[2], "/bundle/lib:/bundle/sysroot/lib/i386-linux-gnu")

    def test_capture_reserves_report_before_execution_and_records_timeout(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            status_path, status = self.target(folder / "sys")
            report = folder / "report.json"
            argv = ["run", "--bundle", str(folder), "--capture", "--report", str(report)]
            manifest = {"transport_version": "0.1.3", "artifact_sha256": {"fixture": "mock"}}
            def child(*args, **kwargs):
                self.assertEqual(json.loads(report.read_text())["outcome"], "capture_pending")
                self.assertNotIn("LD_PRELOAD", kwargs["env"])
                return runner.subprocess.CompletedProcess(args[0], 3, "", "BRIDGE command=6 in=24 out=76\n")
            with patch.object(runner, "verify_bundle", return_value=manifest), \
                 patch.object(runner, "check_capture_target", return_value={"status": status}), \
                 patch.object(runner, "services_status", return_value={**status, "init_ran": "1"}), \
                 patch.object(runner.sys, "argv", argv), \
                 patch.object(runner.subprocess, "run", side_effect=child) as run, \
                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(runner.main(), 0)
                result = json.loads(report.read_text())
                self.assertEqual(result["bridge_commands"], [6])
                self.assertTrue(result["resources_clean"])
                run.reset_mock()
                with self.assertRaises(SystemExit):
                    runner.main()  # Existing reports must never be overwritten.
                run.assert_not_called()
                argv[-1] = str(folder / "timeout.json")
                run.side_effect = runner.subprocess.TimeoutExpired("fixture", 25, output=b"partial", stderr=b"stopped")
                self.assertEqual(runner.main(), 1)
                result = json.loads(Path(argv[-1]).read_text())
                self.assertEqual(result["outcome"], "inspect_report")
                self.assertIsNone(result["initializer_process_exit"])
                self.assertEqual(result["stdout"], "partial")


if __name__ == "__main__":
    unittest.main()
