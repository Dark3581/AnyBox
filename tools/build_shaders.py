import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import shader_store

ROOT = Path(__file__).resolve().parent.parent
FORMAT = "anybox-shader-build"
VERSION = 1
SUMMARY = "build.json"
TRANSLATOR_ENTRY = "make_game.py"
STEPS = ("translate", "store", "pipelines")
WAVE64_MODES = ("auto", "on", "off")

STAGE_MODULES = re.compile(r"^(\w+): (\d+) modules ok, (\d+) failed$")
STAGE_PIPELINES = re.compile(r"^(\w+) pipelines: (\d+) ok, (\d+) failed, (\d+) skipped$")
PROBLEM = re.compile(r"^(PIPELINE FAIL|MODULE FAIL|BIG SKIP|SKIPPED) (\S+)")
TOTALS = ("modules_ok", "modules_failed", "pipelines_ok", "pipelines_failed", "pipelines_skipped")


class BuildError(Exception):
    pass


def layout(out):
    work = out / "work"
    return {"work": work, "carved": work / "shaders" / "dxbc", "translated": work / "shaders" / "spirv", "store": out / "store",
            "cache": out / "pipeline_cache.bin", "summary": out / SUMMARY, "translate_log": out / "translate.log",
            "pipelines_log": out / "pipelines.log"}


def translator_command(translator, package, paths, jobs, wave64):
    command = [sys.executable, "-I", str(translator / TRANSLATOR_ENTRY), "--game", str(package), "--work", str(paths["work"]),
               "--out", str(paths["work"] / "port"), "--stages", "shaders", "--wave64-emulate", wave64]
    return command + (["--jobs", str(jobs)] if jobs else [])


def pipeline_command(checker, paths, jobs):
    return [str(checker), str(paths["translated"]), str(paths["cache"])] + (["--jobs", str(jobs)] if jobs else [])


def parse_pipelines(text):
    stages, problems, complete = {}, [], False
    for line in text.splitlines():
        line = line.rstrip()
        if match := STAGE_MODULES.match(line):
            stage = stages.setdefault(match[1], {})
            stage["modules_ok"], stage["modules_failed"] = int(match[2]), int(match[3])
        elif match := STAGE_PIPELINES.match(line):
            stage = stages.setdefault(match[1], {})
            stage["pipelines_ok"], stage["pipelines_failed"], stage["pipelines_skipped"] = int(match[2]), int(match[3]), int(match[4])
        elif match := PROBLEM.match(line):
            problems.append({"kind": match[1].lower().replace(" ", "_"), "name": match[2]})
        elif line.startswith("wall time:"):
            complete = True
    totals = {key: sum(stage.get(key, 0) for stage in stages.values()) for key in TOTALS}
    return {"complete": complete, "stages": stages, "totals": totals, "problems": problems}


def pipelines_passed(result):
    totals = result["totals"]
    return (result.get("exit_code") == 0 and result["complete"] and totals["modules_failed"] == 0 and totals["pipelines_failed"] == 0
            and not any(problem["kind"] in ("pipeline_fail", "module_fail", "big_skip") for problem in result["problems"]))


def run_logged(command, log, env=None):
    log.parent.mkdir(parents=True, exist_ok=True)
    with open(log, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("$ " + subprocess.list2cmdline([str(part) for part in command]) + "\n")
        with subprocess.Popen([str(part) for part in command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                              encoding="utf-8", errors="replace", env=env) as process:
            for line in process.stdout:
                sys.stdout.write(line)
                sys.stdout.flush()
                handle.write(line)
                handle.flush()
            return process.wait()


def read_json(path, default=None):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return default
    except (OSError, ValueError) as error:
        raise BuildError(f"{path}: {error}")


def translator_revision(translator):
    def git(*args):
        result = subprocess.run(["git", "-C", str(translator), *args], capture_output=True, text=True)
        return result.stdout.strip() if result.returncode == 0 else None
    try:
        revision = git("rev-parse", "HEAD")
        changes = git("status", "--porcelain", "--untracked-files=no")
    except OSError:
        return {"revision": None, "modified": None}
    return {"revision": revision, "modified": None if changes is None else bool(changes)}


def count_shaders(paths):
    carve = read_json(paths["carved"] / "index.json")
    translation = read_json(paths["translated"] / "index.json")
    if not isinstance(carve, list) or not isinstance(translation, dict):
        raise BuildError("the translator left no carve index.json list or translation index.json map")
    mode = paths["translated"] / "wave64_emulate.txt"
    return {"carved": len(carve), "translated": len(translation),
            "wave64_emulate": mode.read_text(encoding="utf-8").strip() if mode.is_file() else None}


def build_store(paths):
    modules, duplicates, problems = shader_store.collect(paths["carved"].resolve(), paths["translated"].resolve())
    if not modules:
        raise BuildError("no shader could be stored")
    shader_store.write(paths["store"].resolve(), modules, paths["translated"].resolve())
    return {"modules": len(modules), "duplicates": duplicates, "missing": len(problems["missing"]),
            "invalid": len(problems["invalid"]), "conflicts": len(problems["conflicts"])}


def store_passed(store):
    return store["missing"] == 0 and store["invalid"] == 0 and store["conflicts"] == 0


def save(paths, summary):
    summary["updated"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    paths["summary"].parent.mkdir(parents=True, exist_ok=True)
    temporary = paths["summary"].with_suffix(".partial")
    temporary.write_text(json.dumps(summary, indent=1) + "\n", encoding="utf-8")
    os.replace(temporary, paths["summary"])


def parse_steps(text):
    steps = [step.strip() for step in text.split(",") if step.strip()]
    unknown = [step for step in steps if step not in STEPS]
    if unknown or not steps:
        raise BuildError(f"unknown steps {unknown or text!r}; choose from {','.join(STEPS)}")
    return [step for step in STEPS if step in steps]


def main(argv=None):
    parser = argparse.ArgumentParser(description="Build a title's shader set from its package with an external translator, import it into the shader store, and optionally create every pipeline on this GPU. The output stays in the build folder and is never committed.")
    parser.add_argument("--package", type=Path, required=True, help="the title's package folder")
    parser.add_argument("--translator", type=Path, required=True, help=f"checkout of the external translator (the folder holding {TRANSLATOR_ENTRY})")
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "shaders", help="output folder (default build/shaders)")
    parser.add_argument("--steps", default="translate,store", help=f"comma list from {','.join(STEPS)} (default translate,store)")
    parser.add_argument("--jobs", type=int, default=0, help="worker count passed to the translator and the pipeline check (default: theirs)")
    parser.add_argument("--wave64-emulate", choices=WAVE64_MODES, default="auto", help="the translator's wave64 emulation mode (default auto)")
    parser.add_argument("--pipeline-check", type=Path, help="pipeline checker executable (default: the translator's runtime/build/vk_pipeline_check.exe)")
    args = parser.parse_args(argv)
    paths = layout(args.out.resolve())
    try:
        steps = parse_steps(args.steps)
        if args.jobs < 0:
            raise BuildError("--jobs must be 0 or more")
        package, translator = args.package.resolve(), args.translator.resolve()
        summary = read_json(paths["summary"], {}) or {}
        summary.update({"format": FORMAT, "version": VERSION, "package": str(package), "translator": {"path": str(translator), **translator_revision(translator)}})
        if "translate" in steps:
            if not package.is_dir():
                raise BuildError(f"{package} is not a folder")
            if not (translator / TRANSLATOR_ENTRY).is_file():
                raise BuildError(f"{translator} holds no {TRANSLATOR_ENTRY}")
            print(f"build_shaders: translating with {translator}", flush=True)
            code = run_logged(translator_command(translator, package, paths, args.jobs, args.wave64_emulate), paths["translate_log"])
            if code != 0:
                summary["translation"] = {"exit_code": code}
                save(paths, summary)
                raise BuildError(f"the translator exited with {code}; see {paths['translate_log']}")
            summary["translation"] = {"exit_code": 0, **count_shaders(paths)}
            summary.pop("store", None)
            summary.pop("pipelines", None)
            save(paths, summary)
        if "store" in steps:
            summary["store"] = build_store(paths)
            save(paths, summary)
            store = summary["store"]
            print(f"build_shaders: {store['modules']} modules stored, {store['duplicates']} duplicates, {store['missing']} missing, "
                  f"{store['invalid']} invalid, {store['conflicts']} conflicts", flush=True)
            if not store_passed(store):
                raise BuildError("the store is incomplete")
        if "pipelines" in steps:
            checker = (args.pipeline_check or translator / "runtime" / "build" / "vk_pipeline_check.exe").resolve()
            if not checker.is_file():
                raise BuildError(f"{checker} not found; build the translator's pipeline checker or pass --pipeline-check")
            if not (paths["translated"] / "index.json").is_file():
                raise BuildError("no translated shaders yet: run the translate step first")
            code = run_logged(pipeline_command(checker, paths, args.jobs), paths["pipelines_log"])
            result = parse_pipelines(paths["pipelines_log"].read_text(encoding="utf-8"))
            result["exit_code"] = code
            summary["pipelines"] = result
            save(paths, summary)
            totals = result["totals"]
            print(f"build_shaders: pipelines {totals['pipelines_ok']} ok, {totals['pipelines_failed']} failed, {totals['pipelines_skipped']} skipped; "
                  f"modules {totals['modules_failed']} failed; exit {code}", flush=True)
            if not pipelines_passed(result):
                raise BuildError(f"the pipeline check did not pass; see {paths['pipelines_log']}")
    except (BuildError, shader_store.StoreError) as error:
        print(f"build_shaders: {error}", file=sys.stderr)
        return 1
    print(f"build_shaders: done; summary in {paths['summary']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
