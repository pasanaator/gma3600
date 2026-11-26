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


def content_trace():
    lines = ["RUNTIME bound=1 queried=1 unbound=1 buffers=11 pages=11 errno=0 gpu_execution=disabled", "NATIVE bound=1 restored=1 ccb_bytes=8192 slots=256 read=0 write=0 roundtrip=1 buffers=11 pages=11 gpu_execution=disabled", "HANDOFF validated=1 staged=1 queried=1 aborted=1 buffers=11 pages=11 errno=0 gpu_execution=disabled"]
    lines.append("OWNED_SUMMARY checked=11 buffers=11 pages=11 errno=0 allocator=native_bm")
    lines.append("BM_SUMMARY checked=11 buffers=11 pages=11 errno=0 gpu_execution=disabled")
    for i in range(11):
        lines.append(f"OWNED index={i} handle={0x100+i:08x} address={0xf2000000+i*4096:08x} bytes=4 pages=1 memory_flags=0202420b alignment=4096 owned=1 registered=1 attached=1")
        lines.append(f"BM index={i} handle={0x100+i:08x} original={0xf2000000+i*4096:08x} alias={0xf3000000+i*4096:08x} bytes=4 pages=1 memory_flags=0202420b passes=2 pt_before=1 pt_after=1")
        lines.append(f"CONTENT index={i} handle={0x100+i:08x} bytes=4 address={0xf2000000+i*4096:08x} memory_flags=0202420b "
                     "baseline=12345678 current=12345678 nonzero=1 padding_nonzero=0 changed=0 mapped=0")
        lines.append(f"MAP_RELEASE handle={0x100+i:08x} unmap=1 address={0x70000000+i*4096:08x} bytes=4096")
    lines.append("CONTENT_SUMMARY checked=11 buffers=11 bytes=44 nonzero_bytes=11 populated=11 changed=0 errno=0 mapped=0")
    return "\n".join(lines) + "\n"


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
            manifest = {"schema": 1, "transport_version": "0.8.0", "artifact_sha256": {}}
            for name in runner.ARTIFACTS:
                file = folder / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_bytes(b"test fixture")
                manifest["artifact_sha256"][name] = hashlib.sha256(file.read_bytes()).hexdigest()
            (folder / "bundle.json").write_text(json.dumps(manifest))
            runner.verify_bundle(folder)
            (folder / "bundle.json").write_text(json.dumps({**manifest, "transport_version": "0.1.0"}))
            with self.assertRaisesRegex(ValueError, "Rebuild.*0.8.0"):
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
            "module/gma3600_sgx_services/version": "0.16.0",
        }.items():
            p = root / file
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(value + "\n")
        driver = root / "bus/pci/drivers/gma3600"
        driver.mkdir(parents=True)
        (root / "bus/pci/devices/0000:00:02.0/driver").symlink_to(driver)
        status = {"state": "prepared", "devices": "1", "gpu_execution": "disabled", "bridge": "enabled", "pages_live": "4", "allocator": "native_bm"}
        status.update({key: "0" for key in ("blocked_hw_accesses", "init_ran", "files", "connections", "bridge_errors",
            "heap_handles", "init_running", "buffers", "buffer_pages", "buffer_maps", "buffer_errors", "map_leases",
            "device_allocations", "device_pages", "device_errors", "stages", "staged_buffers", "staged_pages", "stage_errors", "native_bindings", "native_errors", "bm_errors", "bm_allocations", "bm_pages")})
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
            self.assertEqual(runner.check_capture_target(root, "x86_64", 0)["services_version"], "0.16.0")
            for key, value in (("init_ran", "1"), ("gpu_execution", "enabled"), ("pages_live", "24"),
                               ("buffer_maps", "1"), ("stage_errors", "1"), ("blocked_hw_accesses", "1"),
                               ("allocator", "mmu"), ("bm_allocations", "1"), ("bm_pages", "1")):
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
            with self.assertRaisesRegex(ValueError, "services 0.16.0"):
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

    def test_handoff_report_requires_complete_cpu_cycle(self):
        line = "HANDOFF validated=1 staged=1 queried=1 aborted=1 buffers=11 pages=11 errno=0 gpu_execution=disabled\n"
        self.assertTrue(runner.handoff_status(line)["cpu_cycle_passed"])
        self.assertIsNone(runner.handoff_status("BRIDGE command=6 in=24 out=76\n"))
        for old, new in (("queried=1", "queried=0"), ("aborted=1", "aborted=0"),
                         ("errno=0", "errno=5"), ("buffers=11", "buffers=8"),
                         ("pages=11", "pages=1")):
            self.assertFalse(runner.handoff_status(line.replace(old, new))["cpu_cycle_passed"])
        failed = line.replace("errno=0", "errno=5")
        self.assertFalse(runner.handoff_status(line + failed)["cpu_cycle_passed"])

    def test_first_failure_preserves_observed_allocation_and_ignores_cleanup(self):
        trace = ("BRIDGE command=6 in=24 out=76\n"
                 "ALLOC heap=00000004 flags=0002004f bytes=8328 alignment=32\n"
                 "RESULT command=6 payload_error=0\n"
                 "BRIDGE command=6 in=24 out=76\n"
                 "ALLOC heap=00000003 flags=00000009 bytes=53096 alignment=8\n"
                 "RESULT command=6 errno=95\n"
                 "BRIDGE command=68 in=8 out=8\nRESULT command=68 payload_error=129\n")
        self.assertEqual(runner.first_failed_request(trace), {
            "command": 6, "input_bytes": 24, "output_bytes": 76,
            "heap_handle": 3, "flags": 9, "bytes": 53096, "alignment": 8, "errno": 95})
        self.assertIsNone(runner.first_failed_request("BRIDGE command=6 in=24 out=76\nRESULT command=6 payload_error=0\n"))
        self.assertEqual(runner.first_failed_request("BRIDGE command=27 in=8 out=16\nBLOCK bridge command=27\n")["unsupported"], True)
        handoff = "BRIDGE command=99 in=2068 out=8\nHANDOFF validated=0 staged=0 queried=0 aborted=0 buffers=0 pages=0 errno=71 gpu_execution=disabled\n"
        self.assertEqual(runner.first_failed_request(handoff)["phase"], "cpu_handoff")
        self.assertIsNone(runner.first_failed_request(handoff.replace("errno=71", "errno=0")))

    def test_content_report_requires_all_buffers_stable_and_consistent(self):
        trace = content_trace()
        handoff = runner.handoff_status(trace)
        result = runner.content_status(trace, handoff)
        self.assertTrue(result["verified"])
        self.assertEqual(len(result["buffers"]), 11)
        self.assertEqual(result["summary"]["nonzero_bytes"], 11)
        for old, new in (("current=12345678", "current=12345679"), ("changed=0", "changed=1"),
                         ("handle=00000101", "handle=00000100"), ("bytes=44", "bytes=45"),
                         ("nonzero=1", "nonzero=5"), ("errno=0", "errno=5")):
            content_trace_only = "\n".join(line for line in trace.splitlines() if not line.startswith(("BM", "OWNED")))
            changed = content_trace_only.replace(old, new, 1) if old != "errno=0" else content_trace_only.replace("changed=0 errno=0 mapped=0", "changed=0 errno=5 mapped=0")
            self.assertFalse(runner.content_status(changed, runner.handoff_status(changed))["verified"])
        partial = "\n".join(line for line in trace.splitlines() if not line.startswith("CONTENT index=10 "))
        self.assertFalse(runner.content_status(partial, handoff)["verified"])
        self.assertFalse(runner.content_status("", handoff)["verified"])

    def test_native_cycle_requires_restored_empty_ring_and_matching_stage(self):
        trace = content_trace()
        handoff = runner.handoff_status(trace)
        self.assertTrue(runner.native_cycle_status(trace, handoff)["verified"])
        for old, new in (("bound=1", "bound=0"), ("restored=1", "restored=0"), ("slots=256", "slots=128"),
                         ("read=0 write=0", "read=0 write=1"), ("read=0 write=0", "read=256 write=256"),
                         ("roundtrip=1", "roundtrip=0"), ("ccb_bytes=8192", "ccb_bytes=32"),
                         ("buffers=11 pages=11", "buffers=10 pages=11")):
            self.assertFalse(runner.native_cycle_status(next(line for line in trace.splitlines() if line.startswith("NATIVE ")).replace(old, new, 1), handoff)["verified"])
        self.assertFalse(runner.native_cycle_status("", handoff)["verified"])
        self.assertFalse(runner.native_cycle_status(trace + trace, handoff)["verified"])
        self.assertFalse(runner.native_cycle_status(trace + "NATIVE errno=5\n", handoff)["verified"])
        failure = runner.first_failed_request("BRIDGE command=99 in=2068 out=8\nNATIVE errno=5\n")
        self.assertEqual(failure["phase"], "native_cpu_cycle")
        self.assertEqual(failure["errno"], 5)

    def test_native_runtime_requires_retention_query_and_cleanup(self):
        trace = content_trace(); handoff = runner.handoff_status(trace)
        self.assertTrue(runner.native_runtime_status(trace, handoff)["verified"])
        for old, new in (("bound=1", "bound=0"), ("queried=1", "queried=0"), ("unbound=1", "unbound=0"),
                         ("buffers=11", "buffers=10"), ("pages=11", "pages=39"), ("errno=0", "errno=5")):
            self.assertFalse(runner.native_runtime_status(trace.replace(old, new, 1), handoff)["verified"])
        self.assertFalse(runner.native_runtime_status("", handoff)["verified"])
        self.assertFalse(runner.native_runtime_status(trace + trace, handoff)["verified"])
        self.assertEqual(runner.first_failed_request("BRIDGE command=99 in=2068 out=8\nRUNTIME bound=1 queried=0 unbound=1 buffers=11 pages=11 errno=5 gpu_execution=disabled\n")["phase"], "native_runtime")

    def test_native_bm_requires_matching_buffers_reuse_and_retirement(self):
        trace = content_trace(); handoff = runner.handoff_status(trace)
        contents = runner.content_status(trace, handoff)
        self.assertTrue(runner.bm_status(trace, handoff, contents)["verified"])
        bm_lines = "\n".join(line for line in trace.splitlines() if line.startswith("BM"))
        for old, new in (("alias=f3000000", "alias=f2000000"), ("alias=f3000000", "alias=f3000001"),
                         ("passes=2", "passes=1"), ("pt_after=1", "pt_after=2"), ("bytes=4", "bytes=8"),
                         ("pages=1 ", "pages=2 "), ("handle=00000100", "handle=00000101"), ("errno=0", "errno=5")):
            self.assertFalse(runner.bm_status(bm_lines.replace(old, new, 1), handoff, contents)["verified"])
        self.assertFalse(runner.bm_status("", handoff, contents)["verified"])
        self.assertFalse(runner.bm_status(bm_lines + "\n" + bm_lines, handoff, contents)["verified"])
        self.assertEqual(runner.first_failed_request("BRIDGE command=99 in=2068 out=8\nBM_SUMMARY checked=3 buffers=11 pages=3 errno=5 gpu_execution=disabled\n")["phase"], "native_bm")

    def test_native_original_ownership_requires_handles_alignment_and_matching_content(self):
        trace = content_trace(); handoff = runner.handoff_status(trace)
        contents = runner.content_status(trace, handoff)
        self.assertTrue(runner.ownership_status(trace, handoff, contents)["verified"])
        owned = "\n".join(line for line in trace.splitlines() if line.startswith("OWNED"))
        for old, new in (("owned=1", "owned=0"), ("registered=1", "registered=0"), ("attached=1", "attached=0"),
                         ("alignment=4096", "alignment=8192"), ("address=f2000000", "address=f3000000"),
                         ("bytes=4", "bytes=8"), ("handle=00000100", "handle=00000101"), ("errno=0", "errno=5")):
            # Pick index 1 for alignment, whose GPU address is not 8192-aligned.
            changed = owned.replace("alignment=4096", "alignment=8192", 2) if old == "alignment=4096" else owned.replace(old, new, 1)
            self.assertFalse(runner.ownership_status(changed, handoff, contents)["verified"])
        self.assertFalse(runner.ownership_status("", handoff, contents)["verified"])
        self.assertFalse(runner.ownership_status(owned + "\n" + owned, handoff, contents)["verified"])
        failure = runner.first_failed_request("BRIDGE command=99 in=2068 out=8\nOWNED_SUMMARY checked=3 buffers=11 pages=3 errno=5 allocator=native_bm\n")
        self.assertEqual(failure["phase"], "native_ownership")

    def test_mapping_release_requires_kernel_observed_unmapped_buffers(self):
        trace = content_trace()
        contents = runner.content_status(trace, runner.handoff_status(trace))
        self.assertTrue(runner.mapping_release_status(trace, contents)["all_staged_cpu_mappings_released"])
        for old, new in (("unmap=1", "unmap=0"), ("address=70000000", "address=70000001"),
                         ("bytes=4096", "bytes=8192"), ("MAP_RELEASE handle=00000101", "MAP_RELEASE handle=00000100")):
            self.assertFalse(runner.mapping_release_status(trace.replace(old, new, 1), contents)["all_staged_cpu_mappings_released"])
        mapped = trace.replace("changed=0 mapped=0", "changed=0 mapped=1", 1).replace("errno=0 mapped=0", "errno=0 mapped=1")
        updated = runner.content_status(mapped, runner.handoff_status(mapped))
        self.assertTrue(updated["verified"])
        self.assertFalse(runner.mapping_release_status(mapped, updated)["all_staged_cpu_mappings_released"])
        missing = "\n".join(line for line in trace.splitlines() if not line.startswith("MAP_RELEASE "))
        self.assertFalse(runner.mapping_release_status(missing, contents)["all_staged_cpu_mappings_released"])

    def test_capture_reserves_report_before_execution_and_records_timeout(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            status_path, status = self.target(folder / "sys")
            report = folder / "report.json"
            argv = ["run", "--bundle", str(folder), "--capture", "--report", str(report)]
            manifest = {"transport_version": "0.8.0", "artifact_sha256": {"fixture": "mock"}}
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
                self.assertIsNone(result["handoff"])
                run.reset_mock()
                with self.assertRaises(SystemExit):
                    runner.main()  # Existing reports must never be overwritten.
                run.assert_not_called()
                argv[-1] = str(folder / "handoff.json")
                run.side_effect = None
                run.return_value = runner.subprocess.CompletedProcess([], 3, "", content_trace())
                self.assertEqual(runner.main(), 0)
                result = json.loads(Path(argv[-1]).read_text())
                self.assertEqual(result["outcome"], "native_allocator_verified_with_clean_resources")
                self.assertTrue(result["handoff"]["cpu_cycle_passed"])
                self.assertTrue(result["contents"]["verified"])
                self.assertTrue(result["mapping_releases"]["all_staged_cpu_mappings_released"])
                self.assertTrue(result["native_cycle"]["verified"])
                argv[-1] = str(folder / "missing-native.json")
                run.return_value = runner.subprocess.CompletedProcess([], 3, "", "\n".join(
                    line for line in content_trace().splitlines() if not line.startswith("NATIVE ")))
                self.assertEqual(runner.main(), 1)
                self.assertEqual(json.loads(Path(argv[-1]).read_text())["outcome"], "inspect_report")
                argv[-1] = str(folder / "missing-runtime.json")
                run.return_value = runner.subprocess.CompletedProcess([], 3, "", "\n".join(
                    line for line in content_trace().splitlines() if not line.startswith("RUNTIME ")))
                self.assertEqual(runner.main(), 1)
                self.assertEqual(json.loads(Path(argv[-1]).read_text())["outcome"], "inspect_report")
                run.return_value = runner.subprocess.CompletedProcess([], 3, "", content_trace())
                argv[-1] = str(folder / "missing-bm.json")
                run.return_value = runner.subprocess.CompletedProcess([], 3, "", "\n".join(
                    line for line in content_trace().splitlines() if not line.startswith("BM")))
                self.assertEqual(runner.main(), 1)
                self.assertEqual(json.loads(Path(argv[-1]).read_text())["outcome"], "inspect_report")
                run.return_value = runner.subprocess.CompletedProcess([], 3, "", content_trace())
                argv[-1] = str(folder / "missing-ownership.json")
                run.return_value = runner.subprocess.CompletedProcess([], 3, "", "\n".join(
                    line for line in content_trace().splitlines() if not line.startswith("OWNED")))
                self.assertEqual(runner.main(), 1)
                self.assertEqual(json.loads(Path(argv[-1]).read_text())["outcome"], "inspect_report")
                run.return_value = runner.subprocess.CompletedProcess([], 3, "", content_trace())
                argv[-1] = str(folder / "dirty-handoff.json")
                with patch.object(runner, "services_status", return_value={**status, "stages": "1"}):
                    self.assertEqual(runner.main(), 1)
                self.assertEqual(json.loads(Path(argv[-1]).read_text())["outcome"], "inspect_report")
                argv[-1] = str(folder / "timeout.json")
                run.side_effect = runner.subprocess.TimeoutExpired("fixture", 25, output=b"partial", stderr=b"stopped")
                self.assertEqual(runner.main(), 1)
                result = json.loads(Path(argv[-1]).read_text())
                self.assertEqual(result["outcome"], "inspect_report")
                self.assertIsNone(result["initializer_process_exit"])
                self.assertEqual(result["stdout"], "partial")


if __name__ == "__main__":
    unittest.main()
