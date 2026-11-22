import unittest

import helpers

import gpu_report


def module(stage, features=(), capabilities=(), push=0, wave64=False, emulated=False):
    return {"stage": stage, "translation": {"features": list(features), "capabilities": list(capabilities), "push_constant_bytes": push,
                                            "wave64": wave64, "wave64_emulated": emulated}}


def device(features, push=256, operations=0xFF, stages=0xFF, min_size=32, max_size=32, required=0x0):
    return {"name": "test", "features": features, "maxPushConstantsSize": push,
            "subgroup": {"size": max_size, "minSize": min_size, "maxSize": max_size, "operations": hex(operations), "stages": hex(stages), "requiredSizeStages": hex(required)}}


MODULES = {
    "a": module("ps", ["bufferDeviceAddress", "shaderInt64"], ["Shader", "GroupNonUniformQuad"], push=144),
    "b": module("ms", ["meshShader", "bufferDeviceAddress"], ["GroupNonUniformBallot"], wave64=True, emulated=True),
    "c": module("cs", [], ["GroupNonUniformVote", "GroupNonUniformPartitionedNV"], wave64=True),
}


class GpuReportTest(unittest.TestCase):
    def test_requirements(self):
        needed = gpu_report.requirements(MODULES)
        self.assertEqual(dict(needed["features"]), {"bufferDeviceAddress": 2, "shaderInt64": 1, "meshShader": 1})
        self.assertEqual(needed["pushConstants"], 144)
        self.assertEqual(needed["subgroup"], {(0x80, 0x10): 1, (0x8, 0x80): 1, (0x2, 0x20): 1})
        self.assertEqual(dict(needed["unknownSubgroup"]), {"GroupNonUniformPartitionedNV": 1})
        self.assertEqual(dict(needed["nativeWave64"]), {0x20: 1})

    def test_evaluate_a_capable_device(self):
        needed = gpu_report.requirements({k: v for k, v in MODULES.items() if k != "c"})
        full = device({"bufferDeviceAddress": True, "shaderInt64": True, "meshShader": True})
        self.assertEqual(gpu_report.evaluate(full, needed), [])

    def test_evaluate_reports_each_gap(self):
        needed = gpu_report.requirements(MODULES)
        weak = device({"bufferDeviceAddress": True, "shaderInt64": False}, push=128, operations=0x7F, stages=0x7F)
        problems = gpu_report.evaluate(weak, needed)
        self.assertIn("feature meshShader (1 modules): not queried by anybox-gpu-report", problems)
        self.assertIn("feature shaderInt64 (1 modules): unsupported", problems)
        self.assertIn("push constants: modules use 144 bytes, the device allows 128", problems)
        self.assertIn("subgroup GroupNonUniformQuad in ps shaders (1 modules): unsupported", problems)
        self.assertIn("subgroup GroupNonUniformBallot in ms shaders (1 modules): unsupported", problems)
        self.assertIn("subgroup capability GroupNonUniformPartitionedNV (1 modules): not checked", problems)
        self.assertIn("wave64 without emulation in cs shaders (1 modules): the device cannot run them 64 wide", problems)

    def test_native_wave64_on_a_wide_device(self):
        needed = gpu_report.requirements({"c": module("cs", wave64=True)})
        self.assertEqual(gpu_report.evaluate(device({}, min_size=32, max_size=64, required=0x20), needed), [])
        self.assertEqual(len(gpu_report.evaluate(device({}, min_size=32, max_size=64, required=0x10), needed)), 1)


if __name__ == "__main__":
    unittest.main()
