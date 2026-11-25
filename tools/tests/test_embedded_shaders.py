import json
import struct
import unittest
from pathlib import Path

import helpers
import embedded_shaders


def container(version, extra):
    payload = struct.pack("<II4sIII", version, 0, b"DXIL", 0x106, 16, 4) + b"NOBC" + extra
    table = 32 + 4
    size = table + 8 + len(payload)
    return b"DXBC" + b"\0" * 16 + struct.pack("<HHII", 1, 0, size, 1) + struct.pack("<I", table) + b"DXIL" + struct.pack("<I", len(payload)) + payload


VERTEX = container(0x10066, b"XDAT" + b"\0" * 8 + b"XBCH")
COMPUTE = container(0x50065, b"XDAT" + b"\0" * 8 + b"XBCH")
STUB = container(0x00066, b"")
UNKNOWN_STAGE = container(0x0F0066, b"XBCH")


class EmbeddedShadersTest(unittest.TestCase):
    def test_native_containers_only(self):
        data = b"junk" + VERTEX + b"\0" * 7 + STUB + COMPUTE + UNKNOWN_STAGE + b"DXBC" + b"\0" * 8
        found = list(embedded_shaders.native_containers(data))
        self.assertEqual([(entry["offset"], entry["stage"], entry["sm"]) for entry in found],
                         [(4, "vs", "6.6"), (4 + len(VERTEX) + 7 + len(STUB), "cs", "6.5")])
        self.assertEqual(found[0]["size"], len(VERTEX))
        self.assertEqual(found[0]["parts"], {"DXIL": len(VERTEX) - 44})

    def test_carve_writes_the_index_and_containers(self):
        with helpers.temp_root() as root:
            root = Path(root)
            package = root / "package"
            package.mkdir()
            (package / "Title.exe").write_bytes(b"MZ" + VERTEX)
            (package / "Builder.DLL").write_bytes(COMPUTE + b"pad" + VERTEX)
            (package / "data.rpf").write_bytes(VERTEX)
            (package / "nested").mkdir()
            (package / "nested" / "Other.dll").write_bytes(VERTEX)
            out = root / "out"
            index = embedded_shaders.carve(package, out)
            self.assertEqual([entry["name"] for entry in index], ["Builder_00000000", f"Builder_{len(COMPUTE) + 3:08x}", "Title_00000002"])
            self.assertEqual(json.loads((out / "index.json").read_text(encoding="utf-8")), index)
            self.assertEqual((out / "Title_00000002.dxbc").read_bytes(), VERTEX)
            self.assertEqual({entry["source"] for entry in index}, {"Builder.DLL", "Title.exe"})
            self.assertEqual([entry["stage"] for entry in index], ["cs", "vs", "vs"])
            self.assertFalse((out / "index.json.tmp").exists())

    def test_cli(self):
        with helpers.temp_root() as root:
            root = Path(root)
            self.assertEqual(embedded_shaders.main([str(root / "missing"), str(root / "out")]), 1)
            (root / "package").mkdir()
            (root / "package" / "Title.exe").write_bytes(VERTEX)
            self.assertEqual(embedded_shaders.main([str(root / "package"), str(root / "out")]), 0)
            self.assertTrue((root / "out" / "Title_00000000.dxbc").is_file())


if __name__ == "__main__":
    unittest.main()
