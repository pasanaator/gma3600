# SPDX-License-Identifier: GPL-2.0-only
import importlib.util
from pathlib import Path
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[1] / "tools/sgx545_inventory.py"
spec = importlib.util.spec_from_file_location("sgx545_inventory", MODULE)
inventory = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inventory)


class InventoryTests(unittest.TestCase):
    def test_elf_architectures_and_non_elf(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "candidate.so"
            for bits, machine, name in ((1, 3, "i386"), (2, 62, "x86_64"), (1, 40, "ARM")):
                header = bytearray(20)
                header[:6] = b"\x7fELF" + bytes((bits, 1))
                header[18:20] = machine.to_bytes(2, "little")
                path.write_bytes(header)
                result = inventory.elf_info(path)
                self.assertEqual(result["machine"], name)
                self.assertEqual(result["bits"], 32 if bits == 1 else 64)
                self.assertEqual(result["compatibility"], "not_verified")
            path.write_text("not executable code")
            self.assertEqual(inventory.elf_info(path)["status"], "not_elf")
            self.assertEqual(inventory.elf_info(path.with_name("missing"))["status"], "unreadable")

    def test_fixture_device_binding_resources_and_library_metadata(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            device = root / "sys/bus/pci/devices/0000:00:02.0"
            device.mkdir(parents=True)
            (device / "vendor").write_text("0x8086")
            (device / "device").write_text("0x0be1")
            (device / "resource").write_text("d0100000 d01fffff 200\n0 0 0\ninvalid\n")
            driver = root / "sys/bus/pci/drivers/gma3600"
            driver.mkdir(parents=True)
            (device / "driver").symlink_to(driver)
            for name in ("card0", "renderD128", "card0-VGA-1"):
                node = root / "sys/class/drm" / name
                node.mkdir(parents=True)
                (node / "device").symlink_to(device)
            module = root / "sys/module/gma3600"
            module.mkdir(parents=True)
            (module / "version").write_text("1.0.4")
            lib = root / "usr/lib/i386-linux-gnu/libpvr2d.so"
            lib.parent.mkdir(parents=True)
            header = bytearray(20)
            header[:6] = b"\x7fELF\x01\x01"
            header[18:20] = (3).to_bytes(2, "little")
            lib.write_bytes(header)
            result = inventory.collect(root)
            self.assertEqual(result["gma3600_version"], "1.0.4")
            gpu = result["devices"][0]
            self.assertEqual(gpu["driver"], "gma3600")
            self.assertEqual(gpu["drm_nodes"], ["card0", "renderD128"])
            self.assertEqual(gpu["resources"][0]["size"], 1024 * 1024)
            self.assertEqual(gpu["resources"][1]["size"], 0)
            self.assertEqual(gpu["resources"][2]["status"], "malformed")
            self.assertEqual(result["libraries"][0]["machine"], "i386")
            self.assertEqual(result["assessment"]["userspace_abi_compatibility"], "not_verified")
            (device / "vendor").write_text("0x1234")
            self.assertEqual(inventory.collect(root)["devices"], [])

    def test_empty_snapshot(self):
        with tempfile.TemporaryDirectory() as folder:
            result = inventory.collect(Path(folder))
            self.assertEqual(result["devices"], [])
            self.assertEqual(result["libraries"], [])
            self.assertIsNone(result["gma3600_version"])


if __name__ == "__main__":
    unittest.main()
