# SPDX-License-Identifier: GPL-2.0-only
import importlib.util
from pathlib import Path
import struct
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[1] / "tools"
spec = importlib.util.spec_from_file_location("sgx545_init_abi_audit", TOOLS / "sgx545_init_abi_audit.py")
audit = importlib.util.module_from_spec(spec)
sys.path.insert(0, str(TOOLS))
try:
    spec.loader.exec_module(audit)
finally:
    sys.path.pop(0)


class InitAbiAuditTests(unittest.TestCase):
    def test_section_schema_rejects_truncation_unknown_magic_and_version(self):
        names = audit.field_names()
        values = [0] * len(names)
        values[:2] = [0x53475841, 1]
        encoded = struct.pack("<" + "I" * len(values), *values)
        self.assertEqual(audit.decode_manifest(encoded, names)["version"], 1)
        for invalid in (encoded[:-1], encoded + b"\0", b"BAD!" + encoded[4:],
                        encoded[:4] + struct.pack("<I", 2) + encoded[8:]):
            with self.assertRaises(ValueError):
                audit.decode_manifest(invalid, names)

    def test_native_width_is_translation_requirement_not_a_wire_failure(self):
        reference = {name: 0 for name in audit.field_names()}
        reference.update(pointer_bytes=4, bridge_package_bytes=28, init2_request_bytes=2068,
                         init2_info_offset=8, firmware_host_control_bytes=68,
                         build_options=0x60218, build_option_mask=0xffffffff)
        compat = dict(reference, build_options=0x360218)
        native = dict(compat, pointer_bytes=8, bridge_package_bytes=48,
                      init2_request_bytes=2192, init2_info_offset=16,
                      build_option_mask=0xffffffff ^ 0x300000)
        result = audit.compare(reference, compat, native)
        self.assertTrue(result["selected_i386_wire_layout_matches"])
        self.assertTrue(result["native_requires_translation"])
        self.assertEqual(result["raw_build_option_xor"], 0x300000)
        self.assertTrue(result["masked_build_options_match"])
        self.assertTrue(result["selected_firmware_control_layouts_match"])
        reference.update(connect_reply_bytes=8, device_free_request_bytes=60, device_free_reply_bytes=8,
                         wire_connect_reply_bytes=8, wire_device_free_request_bytes=60, wire_device_free_reply_bytes=8)
        compat.update(connect_reply_bytes=12, device_free_request_bytes=12, device_free_reply_bytes=4,
                      wire_connect_reply_bytes=8, wire_device_free_request_bytes=60, wire_device_free_reply_bytes=8)
        native.update(wire_connect_reply_bytes=8, wire_device_free_request_bytes=60, wire_device_free_reply_bytes=8)
        result = audit.compare(reference, compat, native)
        self.assertFalse(result["selected_i386_wire_layout_matches"])
        self.assertTrue(result["selected_i386_wire_supported"])
        self.assertEqual(len(result["supported_protocol_forms"]), 3)
        compat["wire_connect_reply_bytes"] = 12
        self.assertFalse(audit.compare(reference, compat, native)["selected_i386_wire_supported"])
        compat["wire_connect_reply_bytes"] = 8
        compat["init2_options_offset"] = 123
        self.assertFalse(audit.compare(reference, compat, native)["selected_i386_wire_layout_matches"])
        native["firmware_host_control_bytes"] = 72
        self.assertFalse(audit.compare(reference, reference, native)["selected_firmware_control_layouts_match"])
        native["build_options"] ^= 0x8
        self.assertFalse(audit.compare(reference, reference, native)["masked_build_options_match"])

    def test_saved_build_command_is_only_parsed_for_defines(self):
        command = 'savedcmd_object := compiler -D__KERNEL__ -DMODULE -DKBUILD_MODNAME="test" -D__KBUILD_MODNAME=test -DCC_USING_FENTRY -DSGX545 -DSGX_CORE_REV=10131 -DPVR_ABI_COMPAT=1,7,862890 ; other-command\n'
        self.assertEqual(audit.public_defines(command), ["SGX545", "SGX_CORE_REV=10131", "PVR_ABI_COMPAT=1,7,862890"])


if __name__ == "__main__":
    unittest.main()
