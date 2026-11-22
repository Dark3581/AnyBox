import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[3] / "tools"
sys.path.insert(0, str(TOOLS))

import build_shaders
import spirv_layout

COMMAND_LINE = 30000
COMPUTE_SAMPLE = 60
COMPUTE_SAMPLE_BYTES = 1 << 20
PSO_SAMPLE = 12


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
            assert len(row) == 5, f"{stem}: not in the store"
            expected = translation[stem]["stage"]
            assert row[1] == expected, f"{stem}: stage {row[1]}, translation says {expected}"
            subgroup = translation[stem].get("subgroup_size") or 0
            assert row[2] == str(subgroup), f"{stem}: subgroup {row[2]}, translation says {subgroup}"
            module = Path(row[3])
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


def store_modules(layout):
    details = json.loads((layout["store"] / "modules.json").read_text(encoding="utf-8"))["modules"]
    return [(layout["store"] / module["file"], module) for module in details.values()]


def check_layout(args, layout):
    run = subprocess.run([str(args.pipeline_probe), "layout"], capture_output=True, text=True, timeout=60)
    assert run.returncode == 0, run
    title = json.loads(run.stdout)
    misfits, push = [], 0
    modules = store_modules(layout)
    for path, module in modules:
        try:
            problems = spirv_layout.mismatches(spirv_layout.descriptor_bindings(path.read_bytes()), title)
        except (OSError, spirv_layout.LayoutError) as error:
            problems = [str(error)]
        bytes_used = module["translation"].get("push_constant_bytes") or 0
        push = max(push, bytes_used)
        if bytes_used > title["pushConstantBytes"]:
            problems.append(f"{bytes_used} push constant bytes, the layout has {title['pushConstantBytes']}")
        misfits += [f"{module['name']}: {problem}" for problem in problems]
    for misfit in misfits[:20]:
        print(misfit)
    assert not misfits, f"{len(misfits)} problems"
    print(f"layout: {len(modules)} of {len(modules)} modules fit the title layout; largest push constant use {push} bytes")


def check_compute(args, layout):
    compute = sorted(((path.stat().st_size, path, module) for path, module in store_modules(layout) if module["stage"] == "cs" and path.stat().st_size <= COMPUTE_SAMPLE_BYTES),
                     key=lambda item: (item[0], item[2]["name"]))
    assert compute, "the store has no compute module"
    step = max(1, len(compute) // COMPUTE_SAMPLE)
    sample = compute[::step][:COMPUTE_SAMPLE]
    jobs = layout["summary"].parent / "compute_sample.txt"
    rows = [f"{m['name']}\t{m['translation'].get('subgroup_size') or 0}\t{path}\n" for _, path, m in sample]
    jobs.write_text("".join(rows), encoding="utf-8", newline="\n")
    features = sorted({feature for _, _, m in sample for feature in m["translation"].get("features", [])})
    command = [str(args.pipeline_probe), "compile", "--jobs-file", str(jobs), "--workers", "4", "--cache", str(layout["summary"].parent / "compute_sample_cache.bin")]
    if features:
        command += ["--features", ",".join(features)]
    if any(m["translation"].get("needs_maintenance8") for _, _, m in sample):
        command.append("--maintenance8")
    run = subprocess.run(command + ["--validate"], capture_output=True, text=True, timeout=1500)
    if run.returncode == 4:
        print("validation layer not installed: compiling without it")
        run = subprocess.run(command, capture_output=True, text=True, timeout=1500)
    if run.returncode == 3:
        print(f"SKIP compute check: {run.stdout.strip()}")
        return
    lines = run.stdout.splitlines()
    for line in lines:
        if not line.startswith("OK\t"):
            print(line)
    assert run.returncode == 0, ("compute pipelines failed", run.stderr[-2000:])
    print(f"compute: {sum(line.startswith('OK') for line in lines)} of {len(sample)} sampled compute modules (of {len(compute)} up to {COMPUTE_SAMPLE_BYTES >> 20} MiB) compiled")


def pso_rows(output):
    found = []
    for line in output.splitlines():
        fields = line.split("\t")
        if len(fields) == 6 and fields[1] in ("ready", "compiled", "refused", "failed"):
            found.append({"container": fields[0], "origin": fields[1], "bytes": int(fields[3]), "seconds": float(fields[4]), "error": fields[5]})
    return found


def check_pso(args, layout, summary):
    compute = sorted(((path.stat().st_size, path, module) for path, module in store_modules(layout) if module["stage"] == "cs"), key=lambda item: (item[0], item[2]["name"]))
    assert compute, "the store has no compute module"
    small = [item for item in compute if item[0] <= COMPUTE_SAMPLE_BYTES]
    step = max(1, len(small) // PSO_SAMPLE)
    sample = small[::step][:PSO_SAMPLE] + [compute[-1]]
    containers = [layout["carved"] / f"{module['name']}.dxbc" for _, _, module in sample]
    for container in containers:
        assert container.is_file(), f"{container} is missing"
    work = layout["summary"].parent
    listing = work / "pso_sample.txt"
    listing.write_text("".join(f"{container}\n" for container in containers), encoding="utf-8", newline="\n")
    features = sorted({feature for _, _, m in sample for feature in m["translation"].get("features", [])})
    base = [str(args.pso_probe), "compute", str(layout["store"]), str(listing), "--limit", str(COMPUTE_SAMPLE_BYTES), "--threads", "4"]
    if features:
        base += ["--features", ",".join(features)]
    if any(m["translation"].get("needs_maintenance8") for _, _, m in sample):
        base.append("--maintenance8")
    env = dict(os.environ, PATH=str(args.libs) + os.pathsep + os.environ.get("PATH", ""), ANYBOX_NO_REPORT="1")
    largest = str(containers[-1])
    print(f"pso: {len(sample) - 1} compute shaders up to {COMPUTE_SAMPLE_BYTES >> 20} MiB and the largest, {sample[-1][2]['name']} ({sample[-1][0]} bytes), "
          f"with a run-time compile limit of {COMPUTE_SAMPLE_BYTES} bytes")
    fresh = work / "pso_fresh_cache.bin"
    fresh.unlink(missing_ok=True)
    run = subprocess.run(base + ["--cache", str(fresh)], capture_output=True, text=True, env=env, timeout=1500)
    if run.returncode == 3:
        print(f"SKIP pso check: {run.stdout.strip()}")
        return
    assert run.returncode == 0, (run.stdout[-3000:], run.stderr[-2000:])
    found = pso_rows(run.stdout)
    assert len(found) == len(containers), run.stdout[-3000:]
    for row in found:
        print(f"  empty cache: {Path(row['container']).stem} {row['origin']} {row['seconds']:.3f}s {row['error']}")
    assert all(row["origin"] in ("ready", "compiled") for row in found[:-1]), "a sampled compute pipeline failed through the shader store"
    assert found[-1]["origin"] in ("ready", "refused"), f"the largest module was {found[-1]['origin']} although it is over the run-time compile limit"
    cache = layout["cache"]
    if not cache.is_file():
        print(f"no offline cache at {cache}: run the shader_pipelines target to fill it")
        return
    run = subprocess.run(base + ["--cache", str(cache)], capture_output=True, text=True, env=env, timeout=1500)
    assert run.returncode == 0, (run.stdout[-3000:], run.stderr[-2000:])
    print("\n".join(line for line in run.stdout.splitlines() if line.startswith("cache seeded")))
    found = pso_rows(run.stdout)
    assert len(found) == len(containers) and all(row["origin"] != "failed" for row in found), run.stdout[-3000:]
    ready = sum(row["origin"] == "ready" for row in found)
    print(f"  offline cache: {ready} of {len(found)} ready, largest {found[-1]['origin']}")
    pipelines = summary.get("pipelines") or {}
    if pipelines and build_shaders.pipelines_passed(pipelines):
        assert ready == len(found), "the offline pipeline check passed, so every sampled pipeline should come from its cache"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--shaders", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--libs", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--pipeline-probe", type=Path, required=True)
    parser.add_argument("--pso-probe", type=Path, required=True)
    parser.add_argument("--check", choices=("store", "gpu", "layout", "compute", "pso", "pipelines"), required=True)
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
    elif args.check == "layout":
        check_layout(args, layout)
    elif args.check == "compute":
        check_compute(args, layout)
    elif args.check == "pso":
        check_pso(args, layout, summary)
    else:
        pipelines = summary.get("pipelines")
        if pipelines is None:
            print(f"SKIP pipeline check not run: cmake --build {args.build_dir} --target shader_pipelines")
            return
        totals = pipelines["totals"]
        print(f"pipelines: {totals}, exit {pipelines.get('exit_code')}, complete {pipelines['complete']}")
        for problem in pipelines["problems"][:20]:
            print(f"  {problem['kind']} {problem['name']}")
        for failed in pipelines.get("failed_chunks", []):
            print(f"  chunk [{failed['start']}, {failed['start'] + failed['count']}) exit {failed['exit_code']}, last named compile {failed['last_compiling']}")
        assert build_shaders.pipelines_passed(pipelines), "the last pipeline check did not pass"
    print(f"PASS shaders_{args.check}")


if __name__ == "__main__":
    main()
