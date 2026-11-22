import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

EXPECTED = {
    "fake_module_ok.dll": {"result": "loaded"},
    "fake_module_fail.dll": {"result": "failed", "error": 1114},
    "fake_module_stop.dll": {"result": "stopped", "kind": "not-implemented", "library": "d3d12_x", "function": "D3D12XboxCreateDevice", "message": None},
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--libs", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("modules", type=Path, nargs="+")
    args = parser.parse_args()

    shutil.rmtree(args.work, ignore_errors=True)
    title = args.work / "title"
    (title / "bin").mkdir(parents=True)
    for module in args.modules:
        destination = title / ("bin" if "crash" in module.name else ".") / module.name
        shutil.copyfile(module, destination)
        destination.with_name(destination.name + ".registry.json").write_text("{}\n", encoding="utf-8")
    shutil.copyfile(args.modules[0], title / "unconverted.dll")
    for library in ("ab_runtime.dll", "ab_d3d12_x.dll"):
        shutil.copyfile(args.libs / library, title / library)
    shutil.copyfile(args.libs / "ab_runtime.dll", title / "bin" / "ab_runtime.dll")

    output = args.work / "results.json"
    run = subprocess.run([sys.executable, str(args.tool), str(title), "--build-dir", str(args.build_dir), "--json", str(output), "--timeout", "30"],
                         capture_output=True, text=True, timeout=300)
    print(run.stdout)
    print(run.stderr, file=sys.stderr)
    assert run.returncode == 1, f"exit code {run.returncode}, expected 1"
    results = json.loads(output.read_text(encoding="utf-8"))
    assert sorted(results) == ["bin/fake_module_crash.dll", "fake_module_fail.dll", "fake_module_ok.dll", "fake_module_stop.dll"], results
    for name, expected in EXPECTED.items():
        assert results[name] == expected, (name, results[name])
    crash = results["bin/fake_module_crash.dll"]
    assert crash["result"] == "failed" and crash["error"] == 1114, crash
    assert crash["exception"] == "0xC0000005" and crash["module"] == "fake_module_crash.dll" and crash["offset"].startswith("0x"), crash
    assert "3 of 4" not in run.stdout and "1 of 4 modules loaded" in run.stdout, run.stdout
    assert not (title / "anybox-module-probe.exe").exists(), "the probe is removed again"
    print("PASS module_check")


if __name__ == "__main__":
    main()
