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
    for name, data, stage, spirv, *subgroup in shaders:
        (carved / f"{name}.dxbc").write_bytes(data)
        carve.append({"name": name, "file": f"{name}.dxbc"})
        if spirv is not None:
            (translated / f"{name}.spv").write_bytes(spirv)
            translation[name] = {"stage": stage, **({"subgroup_size": subgroup[0]} if subgroup else {})}
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
            carved, translated = shader_set(root, [("a", container(1), "ps", b"A", 64), ("b c", container(2), "ms", b"B")])
            modules, _, _ = shader_store.collect(carved, translated)
            store = root / "store"
            shader_store.write(store, modules)
            entries = shader_store.read_index(store)
            self.assertEqual({(e["stage"], e["subgroup"]) for e in entries.values()}, {("ps", 64), ("ms", 0)})
            for key, entry in entries.items():
                self.assertTrue((store / entry["file"]).is_file())
                self.assertEqual(entry["file"].split("/")[0], "..")
            details = json.loads((store / "modules.json").read_text(encoding="utf-8"))
            self.assertEqual((details["format"], details["version"]), ("anybox-shader-store", 3))
            self.assertEqual(sorted(d["name"] for d in details["modules"].values()), ["a", "b c"])

    def test_subgroup_sizes(self):
        with helpers.temp_root() as root:
            root = Path(root)
            carved, translated = shader_set(root, [("w32", container(1), "cs", b"A", 32), ("w64", container(2), "ps", b"B", 64),
                                                   ("any", container(3), "cs", b"C", None), ("odd", container(4), "cs", b"D", 16),
                                                   ("flag", container(5), "cs", b"E", True), ("w32_copy", container(1), "cs", b"A", 64)])
            modules, duplicates, problems = shader_store.collect(carved, translated)
            self.assertEqual(sorted((m["name"], m["subgroup"]) for m in modules.values()), [("any", 0), ("w64", 64)])
            self.assertEqual(sorted(problems["invalid"]), ["flag", "odd"])
            self.assertEqual(problems["conflicts"], [hashlib.sha256(container(1)).hexdigest()])
            self.assertEqual(duplicates, 1)

    def test_vertex_inputs(self):
        signature = [{"register": 0, "semantic": "POSITION", "semantic_index": 0, "mask": 7, "system_value": 0},
                     {"register": 1, "semantic": "TEXCOORD", "semantic_index": 2, "mask": 3, "system_value": 0},
                     {"register": 2, "semantic": "SV_VertexID", "semantic_index": 0, "mask": 1, "system_value": 6}]
        details = {"input_signature": signature, "vertex_inputs": [{"location": 1, "type": "float"}, {"location": 0, "type": "float"}]}
        self.assertEqual(shader_store.vertex_inputs(details), [
            {"location": 0, "semantic": "POSITION", "index": 0, "type": "float", "mask": 7},
            {"location": 1, "semantic": "TEXCOORD", "index": 2, "type": "float", "mask": 3}])
        self.assertEqual(shader_store.vertex_inputs({"input_signature": signature}), [])
        for broken in ([{"location": 2, "type": "uint"}], [{"location": 5, "type": "float"}], [{"location": 0, "type": "half"}],
                       [{"location": 0, "type": "float"}, {"location": 0, "type": "float"}]):
            self.assertIsNone(shader_store.vertex_inputs({"input_signature": signature, "vertex_inputs": broken}), broken)
        odd = [dict(signature[0], semantic="POS ITION")]
        self.assertIsNone(shader_store.vertex_inputs({"input_signature": odd, "vertex_inputs": [{"location": 0, "type": "float"}]}))
        with helpers.temp_root() as root:
            root = Path(root)
            carved, translated = shader_set(root, [("vs", container(1), "vs", b"A"), ("cs", container(2), "cs", b"B")])
            index = json.loads((translated / "index.json").read_text(encoding="utf-8"))
            index["vs"].update(details)
            (translated / "index.json").write_text(json.dumps(index), encoding="utf-8")
            modules, _, problems = shader_store.collect(carved, translated)
            store = root / "store"
            shader_store.write(store, modules)
            lines = (store / "inputs.txt").read_text(encoding="utf-8").splitlines()
            key = hashlib.sha256(container(1)).hexdigest()
            self.assertEqual(lines, [f"{key}\t0\tPOSITION\t0\tfloat\t7", f"{key}\t1\tTEXCOORD\t2\tfloat\t3"])

    def test_read_index_rejects_bad_lines(self):
        with helpers.temp_root() as root:
            store = Path(root)
            for text in ("abc\tps\t0\tx.spv\n", "0" * 64 + "\tzz\t0\tx.spv\n", "0" * 64 + "\tps\t0\n", "0" * 64 + "\tps\tx.spv\n",
                         "0" * 64 + "\tps\t16\tx.spv\n", "0" * 64 + "\tps\t32\t\n"):
                (store / "modules.txt").write_text(text, encoding="utf-8")
                with self.assertRaises(shader_store.StoreError):
                    shader_store.read_index(store)


if __name__ == "__main__":
    unittest.main()
