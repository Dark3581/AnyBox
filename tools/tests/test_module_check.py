import unittest
from pathlib import Path

import helpers

import module_check


class ModuleCheckTest(unittest.TestCase):
    def test_modules_prefer_converted_and_skip_anybox_libraries(self):
        with helpers.temp_root() as root:
            root = Path(root)
            for name in ("a.dll", "bin/b.dll", "c.dll", "ab_runtime.dll", "data/readme.txt"):
                helpers.write(root, name, "x")
            helpers.write(root, "a.dll.registry.json", "{}")
            helpers.write(root, "bin/b.dll.registry.json", "{}")
            helpers.write(root, "ab_runtime.dll.registry.json", "{}")
            self.assertEqual([p.relative_to(root).as_posix() for p in module_check.modules(root)], ["a.dll", "bin/b.dll"])

    def test_modules_fall_back_to_every_dll(self):
        with helpers.temp_root() as root:
            root = Path(root)
            for name in ("z.dll", "AB_xmem.dll", "sub/y.DLL"):
                helpers.write(root, name, "x")
            self.assertEqual(sorted(p.relative_to(root).as_posix() for p in module_check.modules(root)), ["sub/y.DLL", "z.dll"])

    def test_classify(self):
        self.assertEqual(module_check.classify(0, "loaded\n", None), {"result": "loaded"})
        self.assertEqual(module_check.classify(1, "failed 126\n", None), {"result": "failed", "error": 126})
        self.assertEqual(module_check.classify(1, "failed 1114 exception 0xC0000005 mid.dll+0x1a2b\n", None),
                         {"result": "failed", "error": 1114, "exception": "0xC0000005", "module": "mid.dll", "offset": "0x1a2b"})
        report = {"stop": {"kind": "unsupported", "library": "xmem", "function": "XMemFree", "message": "address is null"}}
        self.assertEqual(module_check.classify(0xAB02, "", report),
                         {"result": "stopped", "kind": "unsupported", "library": "xmem", "function": "XMemFree", "message": "address is null"})
        self.assertEqual(module_check.classify(0xAB01, "", None),
                         {"result": "stopped", "kind": "not-implemented", "library": None, "function": None, "message": None})
        crash = {"stop": {"kind": "crash", "exception": {"code": "0xc0000005", "module": "m.dll", "offset": "0x10"}}}
        self.assertEqual(module_check.classify(-1073741819, "", crash), {"result": "crashed", "exitCode": "0xC0000005", "module": "m.dll", "offset": "0x10"})
        self.assertEqual(module_check.classify(0, "", None), {"result": "crashed", "exitCode": "0x00000000"})

    def test_describe(self):
        self.assertEqual(module_check.describe("a.dll", {"result": "loaded"}), "a.dll: loaded")
        self.assertEqual(module_check.describe("a.dll", {"result": "failed", "error": 126}), "a.dll: failed to load, Win32 error 126")
        self.assertEqual(module_check.describe("a.dll", {"result": "failed", "error": 1114, "exception": "0xC0000005", "module": "a.dll", "offset": "0x10"}),
                         "a.dll: failed to load, Win32 error 1114 after exception 0xC0000005 in a.dll+0x10")
        self.assertEqual(module_check.describe("a.dll", {"result": "stopped", "kind": "not-implemented", "library": "d3d12_x", "function": "D3D12XboxCreateDevice", "message": None}),
                         "a.dll: stopped (not-implemented) in d3d12_x!D3D12XboxCreateDevice")
        self.assertEqual(module_check.describe("a.dll", {"result": "crashed", "exitCode": "0xC0000409"}), "a.dll: crashed with 0xC0000409")
        self.assertEqual(module_check.describe("a.dll", {"result": "timeout"}), "a.dll: did not finish loading within the time limit")


if __name__ == "__main__":
    unittest.main()
