import argparse
import json
import subprocess
from pathlib import Path

EXPECTED = {
    0: ("sampled_image", "2d", False, "float", "unknown"), 1: ("sampler", "none", False, "none", "none"),
    2: ("storage_buffer", "none", False, "none", "none"), 3: ("sampled_image", "2d", True, "float", "unknown"),
    4: ("storage_image", "2d", False, "uint", "r32ui"), 5: ("sampled_image", "1d", False, "float", "unknown"),
    6: ("sampled_image", "3d", False, "float", "unknown"), 7: ("storage_image", "3d", False, "uint", "r32ui"),
    **{8 + 3 * d + c: ("storage_image", dim, arrayed, component, "unknown")
       for d, (dim, arrayed) in enumerate((("2d", False), ("2d", True), ("3d", False))) for c, component in enumerate(("float", "uint", "sint"))},
    **{17 + c: ("storage_image", "1d", False, component, "unknown") for c, component in enumerate(("float", "uint", "sint"))},
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
    found = {b["binding"]: (b["type"], b["dim"], b["arrayed"], b["component"], b["format"]) for b in layout["bindings"]}
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

    bindless = run([args.probe, "bindless", args.work / "bindless", "--validate"])
    if bindless.returncode == 4:
        bindless = run([args.probe, "bindless", args.work / "bindless"])
    assert bindless.returncode == 0 and "PASS bindless" in bindless.stdout and "wrong: 0" in bindless.stdout, (bindless.returncode, bindless.stdout)

    window = run([args.probe, "window", args.work / "window", "--validate"])
    if window.returncode == 4:
        window = run([args.probe, "window", args.work / "window"])
    if window.returncode == 3:
        print("SKIP window check: the device cannot import host memory")
    else:
        assert window.returncode == 0 and "PASS window" in window.stdout and "wrong: 0" in window.stdout, (window.returncode, window.stdout)
    print("PASS pipeline_probe")


if __name__ == "__main__":
    main()
