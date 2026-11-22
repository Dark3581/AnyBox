import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import helpers

import spirv_layout

MODULE = bytes.fromhex(
    "0302230700060100000007001c00000000000000110002000100000011000200b614000011000200380000000a0008005350"
    "565f4558545f64657363726970746f725f696e646578696e67000e00030000000000010000000f0005000500000001000000"
    "6d61696e00000000100006000100000011000000010000000100000001000000470004000200000021000000000000004700"
    "0400020000002200000000000000470004000300000021000000010000004700040003000000220000000000000047000400"
    "0400000021000000020000004700040004000000220000000000000047000400050000002100000003000000470004000500"
    "0000220000000000000047000400060000002100000007000000470004000600000022000000000000004700040007000000"
    "0600000004000000480005000800000000000000230000000000000047000300080000000200000013000200090000002100"
    "03000a00000009000000160003000b00000020000000150004000c00000020000000000000002b0004000c0000000d000000"
    "04000000190009000e0000000b000000010000000000000000000000000000000100000000000000190009000f0000000b00"
    "000001000000000000000100000000000000010000000000000019000900100000000b000000020000000000000000000000"
    "0000000002000000000000001a000200110000001d000300120000000e0000001d00030013000000110000001c0004001400"
    "00000f0000000d0000001d00030015000000100000001d000300070000000c0000001e000300080000000700000020000400"
    "1600000000000000120000002000040017000000000000001300000020000400180000000000000014000000200004001900"
    "00000000000015000000200004001a0000000c000000080000003b0004001600000002000000000000003b00040017000000"
    "03000000000000003b0004001800000005000000000000003b0004001900000006000000000000003b0004001a0000000400"
    "00000c000000360005000900000001000000000000000a000000f80002001b000000fd00010038000100"
)

LAYOUT = {"set": 0, "pushConstantBytes": 144, "bindings": [
    {"binding": 0, "type": "sampled_image", "dim": "2d", "arrayed": False, "component": "float", "format": "unknown", "count": 8192},
    {"binding": 1, "type": "sampler", "dim": "none", "arrayed": False, "component": "none", "format": "none", "count": 2048},
    {"binding": 2, "type": "storage_buffer", "dim": "none", "arrayed": False, "component": "none", "format": "none", "count": 1},
    {"binding": 3, "type": "sampled_image", "dim": "2d", "arrayed": True, "component": "float", "format": "unknown", "count": 2048},
    {"binding": 7, "type": "storage_image", "dim": "3d", "arrayed": False, "component": "float", "format": "unknown", "count": 256},
]}


class SpirvLayoutTest(unittest.TestCase):
    def test_reflects_every_binding(self):
        found = spirv_layout.descriptor_bindings(MODULE)
        self.assertEqual(found, {
            (0, 0): {"type": "sampled_image", "dim": "2d", "arrayed": False, "component": "float", "format": "unknown", "count": None},
            (0, 1): {"type": "sampler", "dim": "none", "arrayed": False, "component": "none", "format": "none", "count": None},
            (0, 2): {"type": "storage_buffer", "dim": "none", "arrayed": False, "component": "none", "format": "none", "count": 1},
            (0, 3): {"type": "sampled_image", "dim": "2d", "arrayed": True, "component": "float", "format": "unknown", "count": 4},
            (0, 7): {"type": "storage_image", "dim": "3d", "arrayed": False, "component": "float", "format": "unknown", "count": None},
        })

    def test_fits_its_layout(self):
        self.assertEqual(spirv_layout.mismatches(spirv_layout.descriptor_bindings(MODULE), LAYOUT), [])

    def test_reports_each_difference(self):
        layout = json.loads(json.dumps(LAYOUT))
        layout["bindings"][3]["arrayed"] = False
        layout["bindings"][3]["count"] = 2
        layout["bindings"][4]["type"] = "sampled_image"
        layout["bindings"][4]["format"] = "r32ui"
        del layout["bindings"][1]
        problems = spirv_layout.mismatches(spirv_layout.descriptor_bindings(MODULE), layout)
        self.assertEqual(problems, [
            "set 0 binding 1 is not in the layout",
            "binding 3: arrayed True, the layout has False",
            "binding 3: 4 descriptors, the layout has 2",
            "binding 7: type storage_image, the layout has sampled_image",
            "binding 7: format unknown, the layout has r32ui",
        ])

    def test_rejects_what_is_not_spirv(self):
        for data, message in ((MODULE[:18], "whole number"), (bytes(20), "magic"), (MODULE[:20] + struct.pack("<I", 0xFFFF0011) + MODULE[24:], "bad instruction length")):
            with self.assertRaises(spirv_layout.LayoutError) as caught:
                spirv_layout.descriptor_bindings(data)
            self.assertIn(message, str(caught.exception))

    def test_command_line(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "layout.json").write_text(json.dumps(LAYOUT), encoding="utf-8")
            (root / "good.spv").write_bytes(MODULE)
            (root / "bad.spv").write_bytes(b"nope")
            run = subprocess.run([sys.executable, str(helpers.TOOLS / "spirv_layout.py"), str(root / "layout.json"), str(root / "good.spv"), str(root / "bad.spv")],
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 1, run.stdout)
            self.assertIn("1 of 2 modules fit the layout", run.stdout)
            self.assertIn("bad.spv: not a whole number of SPIR-V words", run.stdout)


if __name__ == "__main__":
    unittest.main()
