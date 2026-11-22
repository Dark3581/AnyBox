import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[3] / "tools"
sys.path.insert(0, str(TOOLS))

import build_shaders

COMMAND_LINE = 30000


def batches(paths, fixed):
    batch, length = [], fixed
    for path in paths:
        if batch and length + len(str(path)) + 3 > COMMAND_LINE:
            yield batch
            batch, length = [], fixed
        batch.append(path)
        length += len(str(path)) + 3
    if batch:
        yield batch


def check_lookups(args, layout, translation, store):
    carve = json.loads((layout["carved"] / "index.json").read_text(encoding="utf-8"))
    containers = [layout["carved"] / entry["file"] for entry in carve]
    env = dict(os.environ, PATH=str(args.libs) + os.pathsep + os.environ.get("PATH", ""))
    found, shared = 0, 0
    fixed = len(str(args.probe)) + len(str(layout["store"])) + 16
    for batch in batches(containers, fixed):
        run = subprocess.run([str(args.probe), "find", str(layout["store"]), *map(str, batch)], capture_output=True, text=True, env=env, timeout=600)
        assert run.returncode == 0, (run.returncode, run.stdout[-2000:], run.stderr[-2000:])
        rows = [line.split("\t") for line in run.stdout.splitlines()]
        assert len(rows) == len(batch), (len(rows), len(batch))
        for container, row in zip(batch, rows):
            stem = container.stem
            assert len(row) == 3, f"{stem}: not in the store"
            expected = translation[stem]["stage"]
            assert row[1] == expected, f"{stem}: stage {row[1]}, translation says {expected}"
            module = Path(row[2])
            assert module.is_file(), f"{stem}: {module} is missing"
            shared += module.stem != stem
            found += 1
    assert shared == store["duplicates"], f"{shared} containers resolved to another container's module; the store counted {store['duplicates']} duplicates"
    print(f"lookups: {found} of {len(containers)} containers found by the C++ store, {shared} through a shared module")


def check_gpu(args, layout):
    run = subprocess.run([sys.executable, str(TOOLS / "gpu_report.py"), "--build-dir", str(args.build_dir), "--store", str(layout["store"])],
                         capture_output=True, text=True, timeout=300)
    print(run.stdout.strip())
    if run.returncode == 1 and run.stdout.startswith("Vulkan:"):
        print("SKIP gpu check: Vulkan is not available")
        return
    assert run.returncode == 0, ("no Vulkan device supports every stored module", run.stdout, run.stderr)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--shaders", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--libs", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--check", choices=("store", "gpu", "pipelines"), required=True)
    args = parser.parse_args()
    layout = build_shaders.layout(args.shaders)
    summary_path = layout["summary"]
    assert summary_path.is_file(), f"{summary_path} not found: build the shaders first with cmake --build {args.build_dir} --target shaders"
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    assert summary.get("format") == build_shaders.FORMAT and summary.get("version") == build_shaders.VERSION, summary_path
    translation = summary.get("translation", {})
    assert translation.get("exit_code") == 0, f"the last translation failed: {translation}"
    assert "store" in summary, f"no store in {summary_path}: build the shaders target again"
    store = summary["store"]
    print(f"translator {summary['translator'].get('revision')} (modified: {summary['translator'].get('modified')}), wave64 emulation {translation.get('wave64_emulate')}")
    print(f"shaders: {translation['carved']} carved, {translation['translated']} translated; store: {store}")
    if args.check == "store":
        assert translation["translated"] == translation["carved"], "some carved shaders have no translation"
        assert build_shaders.store_passed(store), store
        assert store["modules"] + store["duplicates"] == translation["carved"], store
        index = json.loads((layout["translated"] / "index.json").read_text(encoding="utf-8"))
        check_lookups(args, layout, index, store)
    elif args.check == "gpu":
        check_gpu(args, layout)
    else:
        pipelines = summary.get("pipelines")
        if pipelines is None:
            print(f"SKIP pipeline check not run: cmake --build {args.build_dir} --target shader_pipelines")
            return
        totals = pipelines["totals"]
        print(f"pipelines: {totals}, exit {pipelines.get('exit_code')}, complete {pipelines['complete']}")
        for problem in pipelines["problems"][:20]:
            print(f"  {problem['kind']} {problem['name']}")
        assert build_shaders.pipelines_passed(pipelines), "the last pipeline check did not pass"
    print(f"PASS shaders_{args.check}")


if __name__ == "__main__":
    main()
