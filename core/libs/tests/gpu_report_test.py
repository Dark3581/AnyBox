import argparse
import json
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    run = subprocess.run([str(args.report)], capture_output=True, text=True, timeout=120)
    assert run.returncode in (0, 3), (run.returncode, run.stderr)
    report = json.loads(run.stdout)
    assert report["format"] == "anybox-gpu-report" and report["version"] == 1, report
    assert report["available"] == (run.returncode == 0), report
    assert isinstance(report["instanceVersion"], str) and report["instanceVersion"].count(".") == 2, report
    if not report["available"]:
        assert report["error"], report
        print(f"SKIP device checks: {report['error']}")
        return
    assert report["error"] is None and report["devices"], report
    for device in report["devices"]:
        assert device["name"] and device["type"] in ("integrated", "discrete", "virtual", "cpu", "other"), device
        assert int(device["vendorId"], 16) > 0 and device["maxPushConstantsSize"] >= 128, device
        subgroup = device["subgroup"]
        assert subgroup["size"] >= 1 and int(subgroup["operations"], 16) & 0x1, subgroup
        if tuple(int(part) for part in device["apiVersion"].split(".")) >= (1, 3, 0):
            assert 1 <= subgroup["minSize"] <= subgroup["size"] <= subgroup["maxSize"], subgroup
        for feature in ("bufferDeviceAddress", "shaderInt64", "meshShader", "runtimeDescriptorArray", "subgroupSizeControl"):
            assert isinstance(device["features"].get(feature), bool), (feature, device["features"])
        assert device["extensions"] == sorted(device["extensions"]), "extensions are sorted"
        print(f"{device['name']}: Vulkan {device['apiVersion']}, {sum(device['features'].values())} of {len(device['features'])} features")
    print("PASS gpu_report")


if __name__ == "__main__":
    main()
