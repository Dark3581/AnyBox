import struct
import tempfile
import unittest
import zlib
from pathlib import Path

import helpers
import shader_scan


def container(parts):
    offsets, body = [], b""
    table = 32 + len(parts) * 4
    for fourcc, payload in parts:
        offsets.append(table + len(body))
        body += fourcc + struct.pack("<I", len(payload)) + payload
    size = table + len(body)
    return b"DXBC" + b"\0" * 16 + struct.pack("<HHII", 1, 0, size, len(parts)) + b"".join(struct.pack("<I", o) for o in offsets) + body


def program(bitcode, extra=b""):
    return struct.pack("<II4sIII", 0x60066, 0, b"DXIL", 0x106, 16, len(bitcode)) + bitcode + extra


def deflate(data):
    packer = zlib.compressobj(9, zlib.DEFLATED, -zlib.MAX_WBITS)
    return packer.compress(data) + packer.flush()


def rpf9(entries, header=b"\0" * 60):
    body = b"9FPR" + header
    for name, data, claimed in entries:
        stored = deflate(data)
        size = len(data) if claimed is None else claimed
        body += name + b"\0" + b"\x05" + struct.pack("<IIII", len(stored), 7, len(stored), size) + stored + b"\0" * 3
    return body


NATIVE = program(b"NOBC", b"XDAT" + b"\0" * 8 + b"XBCH")
BITCODE = program(b"BC\xc0\xde" + b"\0" * 8)


class ShaderScanTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def write(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def test_classifies_containers(self):
        self.assertEqual(shader_scan.kind(["ISG1", "DXIL", "HASH"]), "dxil")
        self.assertEqual(shader_scan.kind(["SHEX"]), "dxbc")
        self.assertEqual(shader_scan.kind(["SHDR", "DXIL"]), "mixed")
        self.assertEqual(shader_scan.kind(["RTS0"]), "rootsig")
        self.assertEqual(shader_scan.kind(["ABCD"]), "other")

    def test_finds_embedded_containers_and_skips_false_magic(self):
        dxil = container([(b"DXIL", b"\1" * 12), (b"XHW0", b"\2" * 4)])
        rootsig = container([(b"RTS0", b"\3" * 8)])
        path = self.write("pak/blob.bin", b"junk DXBC not a header" + dxil + b"\0" * 7 + rootsig + b"DXBC")
        found = shader_scan.scan_file(path)
        self.assertEqual([c["kind"] for c in found], ["dxil", "rootsig"])
        self.assertEqual(found[0]["parts"], ["DXIL", "XHW0"])
        self.assertEqual(found[0]["offset"], 22)
        self.assertNotIn("stored", found[0])

    def test_rejects_truncated_container(self):
        path = self.write("cut.cso", container([(b"DXIL", b"\1" * 12)])[:-4])
        self.assertEqual(shader_scan.scan_file(path), [])

    def test_rejects_part_inside_header(self):
        data = bytearray(container([(b"DXIL", b"\1" * 12)]))
        data[32:36] = b"\0\0\0\0"
        self.assertEqual(shader_scan.scan_file(self.write("bad.cso", bytes(data))), [])

    def test_summarizes_a_folder(self):
        self.write("a.cso", container([(b"SHEX", b"\1" * 4)]))
        self.write("sub/b.cso", container([(b"DXIL", b"\1" * 4), (b"XHW0", b"")]))
        self.write("empty.txt", b"")
        files = shader_scan.collect([self.root])
        results = {str(p): c for p in files if (c := shader_scan.scan_file(p))}
        self.assertEqual(shader_scan.summarize(results), {"files": 2, "containers": 2, "kinds": {"dxbc": 1, "dxil": 1}, "parts": {"DXIL": 1, "SHEX": 1, "XHW0": 1},
                                                          "programs": {"unknown": 1}, "stored": {"plain": 2}})

    def test_classifies_dxil_programs(self):
        def classify(payload):
            found = shader_scan.scan_file(self.write("p.cso", container([(b"DXIL", payload)])))
            return found[0]["program"]
        self.assertEqual(classify(BITCODE), "bitcode")
        self.assertEqual(classify(NATIVE), "native")
        self.assertEqual(classify(program(b"NOBC")), "stub")
        self.assertEqual(classify(program(b"ABCD")), "unknown")
        self.assertEqual(classify(b"\1" * 12), "unknown")
        broken = bytearray(BITCODE)
        struct.pack_into("<I", broken, 16, 64)
        self.assertEqual(classify(bytes(broken)), "unknown")

    def test_finds_deflate_entries_in_rpf9(self):
        native = container([(b"PSV0", b"\0" * 4), (b"DXIL", NATIVE), (b"PRIV", b"\0" * 4)])
        bitcode = container([(b"DXIL", BITCODE)])
        data = rpf9([(b"pixel/skin.fxc", native, None), (b"cs_blur", bitcode, None), (b"lies", native, len(native) + 1)])
        found = shader_scan.scan_file(self.write("shaders.rpf", data))
        self.assertEqual([(c.get("name"), c["program"], c["stored"]) for c in found],
                         [("pixel/skin.fxc", "native", "deflate"), ("cs_blur", "bitcode", "deflate")])
        self.assertEqual(found[0]["parts"], ["PSV0", "DXIL", "PRIV"])
        self.assertEqual(found[0]["size"], len(native))
        self.assertEqual(data[found[1]["offset"]:found[1]["offset"] + 1], b"\0")
        summary = shader_scan.summarize({"shaders.rpf": found})
        self.assertEqual(summary["programs"], {"bitcode": 1, "native": 1})
        self.assertEqual(summary["stored"], {"deflate": 2})

    def test_reads_entries_only_in_rpf9_archives(self):
        data = rpf9([(b"name", container([(b"DXIL", program(b"NOBC"))]), None)])
        self.assertEqual(shader_scan.scan_file(self.write("other.bin", b"RPF8" + data[4:])), [])
        self.assertEqual(len(shader_scan.scan_file(self.write("a.rpf", data))), 1)


if __name__ == "__main__":
    unittest.main()
