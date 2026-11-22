import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import run_report_summary

ROOT = Path(__file__).resolve().parent.parent
PROBE = "anybox-module-probe.exe"
STOPS = {0xAB01: "not-implemented", 0xAB02: "unsupported"}


def modules(folder):
    dlls = sorted(p for p in folder.rglob("*") if p.suffix.lower() == ".dll" and p.is_file() and not p.name.lower().startswith("ab_"))
    converted = [p for p in dlls if p.with_name(p.name + ".registry.json").is_file()]
    return converted or dlls


def classify(code, stdout, report):
    code &= 0xFFFFFFFF
    line = stdout.strip().splitlines()[-1] if stdout.strip() else ""
    if code == 0 and line == "loaded":
        return {"result": "loaded"}
    if code == 1 and line.startswith("failed "):
        words = line.split()
        entry = {"result": "failed", "error": int(words[1])}
        if len(words) == 5 and words[2] == "exception" and "+" in words[4]:
            module, offset = words[4].rsplit("+", 1)
            entry.update({"exception": words[3], "module": module, "offset": offset})
        return entry
    stop = (report or {}).get("stop") or {}
    if code in STOPS:
        return {"result": "stopped", "kind": stop.get("kind", STOPS[code]), "library": stop.get("library"), "function": stop.get("function"), "message": stop.get("message")}
    entry = {"result": "crashed", "exitCode": f"0x{code:08X}"}
    exception = stop.get("exception") if stop.get("kind") == "crash" else None
    if exception:
        entry["module"] = exception.get("module")
        entry["offset"] = exception.get("offset")
    return entry


def describe(name, entry):
    result = entry["result"]
    if result == "failed":
        fault = f" after exception {entry['exception']} in {entry['module']}+{entry['offset']}" if entry.get("exception") else ""
        return f"{name}: failed to load, Win32 error {entry['error']}{fault}"
    if result == "stopped":
        where = f" in {entry['library']}!{entry['function']}" if entry.get("function") else ""
        message = f": {entry['message']}" if entry.get("message") else ""
        return f"{name}: stopped ({entry['kind']}){where}{message}"
    if result == "crashed":
        where = f" in {entry['module']}+{entry['offset']}" if entry.get("module") else ""
        return f"{name}: crashed with {entry['exitCode']}{where}"
    if result == "timeout":
        return f"{name}: did not finish loading within the time limit"
    return f"{name}: loaded"


def read_report(path):
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return data if not run_report_summary.validate(data) else None


def check(folder, probe, timeout):
    copied = folder / PROBE
    shutil.copy2(probe, copied)
    results = {}
    try:
        with tempfile.TemporaryDirectory() as reports:
            for module in modules(folder):
                name = module.relative_to(folder).as_posix()
                report = Path(reports) / (module.name + ".json")
                env = dict(os.environ, ANYBOX_REPORT=str(report))
                try:
                    run = subprocess.run([str(copied), str(module)], cwd=module.parent, env=env, capture_output=True, text=True, timeout=timeout)
                except subprocess.TimeoutExpired:
                    results[name] = {"result": "timeout"}
                    continue
                results[name] = classify(run.returncode, run.stdout, read_report(report))
    finally:
        copied.unlink(missing_ok=True)
    return results


def main():
    parser = argparse.ArgumentParser(description="Load every converted DLL of a title, each in its own process, and report which ones load.")
    parser.add_argument("folder", type=Path, help="the converted title folder (the relinker's output with --registry)")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build", help=f"AnyBox build directory holding {PROBE}")
    parser.add_argument("--timeout", type=float, default=60, help="seconds to wait for one module")
    parser.add_argument("--json", type=Path, help="write the results as JSON")
    args = parser.parse_args()
    probe = args.build_dir / PROBE
    if not probe.is_file():
        print(f"{probe} not found; build AnyBox first (docs/dev/BUILD.md)", file=sys.stderr)
        return 2
    folder = args.folder.resolve()
    results = check(folder, probe, args.timeout)
    if not results:
        print(f"no DLLs under {folder}", file=sys.stderr)
        return 2
    for name, entry in results.items():
        print(describe(name, entry))
    loaded = sum(1 for e in results.values() if e["result"] == "loaded")
    print(f"{loaded} of {len(results)} modules loaded")
    if args.json:
        args.json.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    return 0 if loaded == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
