import hashlib
import json
import struct
import unittest
from pathlib import Path

import helpers

import shader_store


def container(fill, size=48):
    data = bytearray(bytes([fill]) * size)
    data[0:4] = b"DXBC"
    struct.pack_into("<I", data, 24, size)
    return bytes(data)


def shader_set(root, shaders):
    carved, translated = root / "carved", root / "translated"
    carved.mkdir()
    translated.mkdir()
    carve, translation = [], {}
    for name, data, stage, spirv in shaders:
        (carved / f"{name}.dxbc").write_bytes(data)
        carve.append({"name": name, "file": f"{name}.dxbc"})
        if spirv is not None:
            (translated / f"{name}.spv").write_bytes(spirv)
            translation[name] = {"stage": stage}
    (carved / "index.json").write_text(json.dumps(carve), encoding="utf-8")
    (translated / "index.json").write_text(json.dumps(translation), encoding="utf-8")
    return carved, translated


class ShaderStoreTest(unittest.TestCase):
    def test_container_hash_covers_the_declared_size(self):
        data = container(7)
        self.assertEqual(shader_store.container_hash(data + b"trailing"), hashlib.sha256(data).hexdigest())
        with self.assertRaises(shader_store.StoreError):
            shader_store.container_hash(b"DXBX" + data[4:])
        with self.assertRaises(shader_store.StoreError):
            shader_store.container_hash(data[:40])

    def test_duplicates_share_a_module_and_conflicts_are_dropped(self):
        with helpers.temp_root() as root:
            root = Path(root)
            carved, translated = shader_set(root, [
                ("a", container(1), "ps", b"A"),
                ("a_copy", container(1), "ps", b"A"),
                ("b", container(2), "vs", b"B"),
                ("b_other", container(2), "vs", b"different"),
                ("c", container(3), "cs", b"C"),
                ("c_stage", container(3), "ps", b"C"),
                ("missing", container(4), "cs", None),
                ("bad", b"not a container at all, long enough..", "ps", b"X"),
            ])
            modules, duplicates, problems = shader_store.collect(carved, translated)
            self.assertEqual(sorted(m["name"] for m in modules.values()), ["a"])
            self.assertEqual(duplicates, 3)
            self.assertEqual(problems["missing"], ["missing"])
            self.assertEqual(problems["invalid"], ["bad"])
            self.assertEqual(sorted(problems["conflicts"]), sorted([hashlib.sha256(container(2)).hexdigest(), hashlib.sha256(container(3)).hexdigest()]))

    def test_index_round_trip(self):
        with helpers.temp_root() as root:
            root = Path(root)
            carved, translated = shader_set(root, [("a", container(1), "ps", b"A"), ("b c", container(2), "ms", b"B")])
            modules, _, _ = shader_store.collect(carved, translated)
            store = root / "store"
            shader_store.write(store, modules, translated)
            entries = shader_store.read_index(store)
            self.assertEqual({e["stage"] for e in entries.values()}, {"ps", "ms"})
            for key, entry in entries.items():
                self.assertTrue((store / entry["file"]).is_file())
                self.assertEqual(entry["file"].split("/")[0], "..")
            details = json.loads((store / "modules.json").read_text(encoding="utf-8"))
            self.assertEqual((details["format"], details["version"]), ("anybox-shader-store", 1))
            self.assertEqual(sorted(d["name"] for d in details["modules"].values()), ["a", "b c"])

    def test_read_index_rejects_bad_lines(self):
        with helpers.temp_root() as root:
            store = Path(root)
            for text in ("abc\tps\tx.spv\n", "0" * 64 + "\tzz\tx.spv\n", "0" * 64 + "\tps\n"):
                (store / "modules.txt").write_text(text, encoding="utf-8")
                with self.assertRaises(shader_store.StoreError):
                    shader_store.read_index(store)


if __name__ == "__main__":
    unittest.main()
