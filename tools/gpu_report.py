import argparse
import json
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPORT = "anybox-gpu-report.exe"

SUBGROUP_OPERATIONS = {
    "GroupNonUniform": 0x1,
    "GroupNonUniformVote": 0x2,
    "GroupNonUniformArithmetic": 0x4,
    "GroupNonUniformBallot": 0x8,
    "GroupNonUniformShuffle": 0x10,
    "GroupNonUniformShuffleRelative": 0x20,
    "GroupNonUniformClustered": 0x40,
    "GroupNonUniformQuad": 0x80,
}
STAGE_BITS = {"vs": 0x1, "hs": 0x2, "ds": 0x4, "gs": 0x8, "ps": 0x10, "cs": 0x20, "as": 0x40, "ms": 0x80}


def requirements(modules):
    features, subgroup, unknown = Counter(), {}, Counter()
    push_constants, native_wave64 = 0, Counter()
    for module in modules.values():
        translation = module.get("translation", {})
        stage = STAGE_BITS.get(module.get("stage"))
        features.update(translation.get("features", []))
        push_constants = max(push_constants, translation.get("push_constant_bytes") or 0)
        for capability in translation.get("capabilities", []):
            bit = SUBGROUP_OPERATIONS.get(capability)
            if bit is not None and stage is not None:
                subgroup[(bit, stage)] = subgroup.get((bit, stage), 0) + 1
            elif capability.startswith("GroupNonUniform"):
                unknown[capability] += 1
        if translation.get("wave64") and not translation.get("wave64_emulated") and stage is not None:
            native_wave64[stage] += 1
    return {"features": features, "subgroup": subgroup, "unknownSubgroup": unknown, "pushConstants": push_constants, "nativeWave64": native_wave64}


def evaluate(device, needed):
    problems = []
    supported = device.get("features", {})
    for feature, count in sorted(needed["features"].items()):
        if supported.get(feature) is not True:
            known = "unsupported" if feature in supported else "not queried by anybox-gpu-report"
            problems.append(f"feature {feature} ({count} modules): {known}")
    if needed["pushConstants"] > device.get("maxPushConstantsSize", 0):
        problems.append(f"push constants: modules use {needed['pushConstants']} bytes, the device allows {device.get('maxPushConstantsSize', 0)}")
    subgroup = device.get("subgroup", {})
    operations, stages = int(subgroup.get("operations", "0x0"), 16), int(subgroup.get("stages", "0x0"), 16)
    for (bit, stage), count in sorted(needed["subgroup"].items()):
        if not operations & bit or not stages & stage:
            operation = next(name for name, value in SUBGROUP_OPERATIONS.items() if value == bit)
            where = next(name for name, value in STAGE_BITS.items() if value == stage)
            problems.append(f"subgroup {operation} in {where} shaders ({count} modules): unsupported")
    for capability, count in sorted(needed["unknownSubgroup"].items()):
        problems.append(f"subgroup capability {capability} ({count} modules): not checked")
    required = int(subgroup.get("requiredSizeStages", "0x0"), 16)
    for stage, count in sorted(needed["nativeWave64"].items()):
        where = next(name for name, value in STAGE_BITS.items() if value == stage)
        if subgroup.get("maxSize", 0) < 64 or not required & stage:
            problems.append(f"wave64 without emulation in {where} shaders ({count} modules): the device cannot run them 64 wide")
    return problems


def run_report(build_dir):
    exe = build_dir / REPORT
    if not exe.is_file():
        raise FileNotFoundError(f"{exe} not found; build AnyBox first (docs/dev/BUILD.md)")
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
    report = json.loads(result.stdout)
    if report.get("format") != "anybox-gpu-report" or report.get("version") != 1:
        raise ValueError("unexpected anybox-gpu-report output")
    return report


def main():
    parser = argparse.ArgumentParser(description="Show the Vulkan devices of this PC and, with --store, whether each one supports what a title's shader store needs.")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build", help=f"AnyBox build directory holding {REPORT}")
    parser.add_argument("--store", type=Path, help="a shader store folder (holding modules.json) to check against")
    parser.add_argument("--json", type=Path, help="write the device report as JSON")
    args = parser.parse_args()
    try:
        report = run_report(args.build_dir)
    except (FileNotFoundError, ValueError, json.JSONDecodeError) as error:
        print(f"gpu_report: {error}", file=sys.stderr)
        return 2
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if not report["available"]:
        print(f"Vulkan: {report['error']}")
        return 1
    needed = None
    if args.store:
        details = json.loads((args.store / "modules.json").read_text(encoding="utf-8"))
        needed = requirements(details["modules"])
        print(f"shader store: {len(details['modules'])} modules")
    print(f"Vulkan instance {report['instanceVersion']}")
    usable = 0
    for device in report["devices"]:
        subgroup = device["subgroup"]
        print(f"{device['name']} ({device['type']}, Vulkan {device['apiVersion']}): subgroup {subgroup['minSize']}-{subgroup['maxSize']}, "
              f"push constants {device['maxPushConstantsSize']} bytes")
        if needed is None:
            continue
        problems = evaluate(device, needed)
        usable += not problems
        print("  supports every module" if not problems else "\n".join(f"  {problem}" for problem in problems))
    return 0 if needed is None or usable else 1


if __name__ == "__main__":
    sys.exit(main())
