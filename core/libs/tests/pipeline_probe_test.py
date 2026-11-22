import argparse
import json
import subprocess
from pathlib import Path

EXPECTED = {
    0: ("sampled_image", "2d", False), 1: ("sampler", "none", False), 2: ("storage_buffer", "none", False),
    3: ("sampled_image", "2d", True), 4: ("storage_image", "2d", False), 5: ("sampled_image", "1d", False),
    6: ("sampled_image", "3d", False), 7: ("storage_image", "3d", False),
    **{8 + i: ("storage_image", "2d", False) for i in range(3)}, **{11 + i: ("storage_image", "2d", True) for i in range(3)},
    **{14 + i: ("storage_image", "3d", False) for i in range(3)}, **{17 + i: ("storage_image", "1d", False) for i in range(3)},
}


def run(command):
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True, timeout=300)
    print(result.stdout.strip())
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()

    layout = json.loads(run([args.probe, "layout"]).stdout)
    assert layout["format"] == "anybox-title-layout" and layout["set"] == 0 and layout["pushConstantBytes"] == 144, layout
    found = {b["binding"]: (b["type"], b["dim"], b["arrayed"]) for b in layout["bindings"]}
    assert found == EXPECTED, found
    assert all(b["count"] >= 1 for b in layout["bindings"]) and next(b for b in layout["bindings"] if b["binding"] == 2)["count"] == 1, layout

    usage = run([args.probe])
    assert usage.returncode == 1 and "usage" in usage.stdout, usage

    selftest = run([args.probe, "selftest", args.work / "selftest"])
    if selftest.returncode == 3:
        print("SKIP device checks: no usable Vulkan device")
        return
    assert selftest.returncode == 0 and "PASS selftest" in selftest.stdout, (selftest.returncode, selftest.stdout, selftest.stderr)

    validation = run([args.probe, "validation", args.work / "validation"])
    if validation.returncode == 4:
        print("validation layer not installed: validation check not run")
    else:
        assert validation.returncode == 0 and "outside the VkPushConstantRange" in validation.stdout, (validation.returncode, validation.stdout)
    print("PASS pipeline_probe")


if __name__ == "__main__":
    main()
