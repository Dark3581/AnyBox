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
STILL_COMPILING = re.compile(r"^\s*still compiling (\S+) after")
TOTALS = ("modules_ok", "modules_failed", "pipelines_ok", "pipelines_failed", "pipelines_skipped")
DEFAULT_CHUNK = 500


class BuildError(Exception):
    pass


def layout(out):
    work = out / "work"
    return {"work": work, "carved": work / "shaders" / "dxbc", "translated": work / "shaders" / "spirv", "store": out / "store",
            "cache": out / "pipeline_cache.bin", "summary": out / SUMMARY, "translate_log": out / "translate.log",
            "pipelines_logs": out / "pipelines"}


def translator_command(translator, package, paths, jobs, wave64):
    command = [sys.executable, "-I", str(translator / TRANSLATOR_ENTRY), "--game", str(package), "--work", str(paths["work"]),
               "--out", str(paths["work"] / "port"), "--stages", "shaders", "--wave64-emulate", wave64]
    return command + (["--jobs", str(jobs)] if jobs else [])


def pipeline_command(checker, paths, jobs, start, count, max_bytes=0):
    command = [str(checker), str(paths["translated"]), str(paths["cache"]), "--start", str(start), "--count", str(count)]
    return command + (["--jobs", str(jobs)] if jobs else []) + (["--max-bytes", str(max_bytes)] if max_bytes else [])


def chunk_ranges(entries, size):
    if entries <= 0:
        return []
    if size <= 0:
        return [(0, entries)]
    return [(start, min(size, entries - start)) for start in range(0, entries, size)]


def chunk_log(paths, start, count):
    return paths["pipelines_logs"] / f"{start:06d}-{start + count:06d}.log"


def checker_identity(checker):
    status = checker.stat()
    return {"path": str(checker), "size": status.st_size, "mtime_ns": status.st_mtime_ns}


def combine_chunks(chunks, entries):
    stages, problems, covered = {}, [], 0
    for chunk in sorted(chunks, key=lambda chunk: chunk["start"]):
        if chunk["start"] == covered:
            covered += chunk["count"]
        for name, counts in chunk["stages"].items():
            stage = stages.setdefault(name, {})
            for key, value in counts.items():
                stage[key] = stage.get(key, 0) + value
        problems.extend(chunk["problems"])
    failed = [{"start": chunk["start"], "count": chunk["count"], "exit_code": chunk["exit_code"], "last_compiling": chunk["last_compiling"]}
              for chunk in chunks if chunk_failed(chunk)]
    exit_codes = [chunk["exit_code"] for chunk in chunks if chunk["exit_code"] != 0]
    totals = {key: sum(stage.get(key, 0) for stage in stages.values()) for key in TOTALS}
    return {"complete": covered == entries and all(chunk["complete"] for chunk in chunks), "stages": stages, "totals": totals,
            "problems": problems, "exit_code": exit_codes[0] if exit_codes else 0, "entries": entries, "failed_chunks": failed}


def parse_pipelines(text):
    stages, problems, complete, last_compiling = {}, [], False, None
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
        elif match := STILL_COMPILING.match(line):
            last_compiling = match[1]
        elif line.startswith("wall time:"):
            complete = True
    totals = {key: sum(stage.get(key, 0) for stage in stages.values()) for key in TOTALS}
    return {"complete": complete, "stages": stages, "totals": totals, "problems": problems, "last_compiling": last_compiling}


def size_skips(result):
    return [problem["name"] for problem in result["problems"] if problem["kind"] == "big_skip"]


def chunk_failed(chunk):
    totals = chunk["totals"]
    return (chunk.get("exit_code") != 0 or not chunk["complete"] or totals["modules_failed"] != 0 or totals["pipelines_failed"] != 0
            or any(problem["kind"] in ("pipeline_fail", "module_fail") for problem in chunk["problems"]))


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
    parser.add_argument("--pipeline-chunk", type=int, default=DEFAULT_CHUNK,
                        help=f"index entries per pipeline checker process (default {DEFAULT_CHUNK}; 0: one process). The driver's compiler keeps its memory until the process exits.")
    parser.add_argument("--pipeline-max-bytes", type=int, default=0,
                        help="report modules larger than this many bytes as BIG SKIP instead of compiling them (default 0: compile all). The check does not pass while any module is skipped.")
    parser.add_argument("--recheck", action="store_true", help="check every chunk again instead of reusing chunks that passed with the same checker")
    args = parser.parse_args(argv)
    paths = layout(args.out.resolve())
    try:
        steps = parse_steps(args.steps)
        if args.jobs < 0:
            raise BuildError("--jobs must be 0 or more")
        if args.pipeline_chunk < 0 or args.pipeline_max_bytes < 0:
            raise BuildError("--pipeline-chunk and --pipeline-max-bytes must be 0 or more")
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
            index = read_json(paths["translated"] / "index.json")
            if not isinstance(index, dict) or not index:
                raise BuildError("the translation index.json is not a non-empty map")
            identity = checker_identity(checker)
            previous = summary.get("pipelines") or {}
            reusable = {}
            if not args.recheck and previous.get("checker") == identity:
                reusable = {(chunk["start"], chunk["count"]): chunk for chunk in previous.get("chunks", []) if pipelines_passed(chunk)}
            chunks = []
            for start, count in chunk_ranges(len(index), args.pipeline_chunk):
                if (start, count) in reusable:
                    chunks.append(reusable[(start, count)])
                    print(f"build_shaders: pipelines [{start}, {start + count}) passed with this checker before; reused", flush=True)
                    continue
                log = chunk_log(paths, start, count)
                began = time.monotonic()
                code = run_logged(pipeline_command(checker, paths, args.jobs, start, count, args.pipeline_max_bytes), log)
                chunk = {"start": start, "count": count, **parse_pipelines(log.read_text(encoding="utf-8")), "exit_code": code,
                         "seconds": round(time.monotonic() - began, 1)}
                chunks.append(chunk)
                totals = chunk["totals"]
                over = len(size_skips(chunk))
                print(f"build_shaders: pipelines [{start}, {start + count}): {totals['pipelines_ok']} ok, {totals['pipelines_failed']} failed, "
                      f"{totals['pipelines_skipped']} skipped" + (f", {over} over the size limit" if over else "") + f"; exit {code}"
                      + (f"; FAILED, see {log}" if chunk_failed(chunk) else ""), flush=True)
                summary["pipelines"] = {**combine_chunks(chunks, len(index)), "checker": identity, "chunk": args.pipeline_chunk, "chunks": chunks}
                save(paths, summary)
            result = {**combine_chunks(chunks, len(index)), "checker": identity, "chunk": args.pipeline_chunk, "chunks": chunks}
            summary["pipelines"] = result
            save(paths, summary)
            totals = result["totals"]
            over = size_skips(result)
            print(f"build_shaders: pipelines {totals['pipelines_ok']} ok, {totals['pipelines_failed']} failed, {totals['pipelines_skipped']} skipped, "
                  f"{len(over)} not checked (over the size limit); modules {totals['modules_failed']} failed; "
                  f"{len(result['failed_chunks'])} of {len(chunks)} chunks failed", flush=True)
            for failed in result["failed_chunks"]:
                print(f"build_shaders:   [{failed['start']}, {failed['start'] + failed['count']}) exit {failed['exit_code']}, "
                      f"last named compile {failed['last_compiling']}", flush=True)
            for name in over:
                print(f"build_shaders:   not checked: {name}", flush=True)
            if result["failed_chunks"] or totals["modules_failed"] or totals["pipelines_failed"]:
                raise BuildError(f"the pipeline check did not pass; chunk logs are in {paths['pipelines_logs']}")
            if not pipelines_passed(result):
                raise BuildError(f"the pipeline check is incomplete: {len(over)} modules over --pipeline-max-bytes were not checked; chunk logs are in {paths['pipelines_logs']}")
    except (BuildError, shader_store.StoreError) as error:
        print(f"build_shaders: {error}", file=sys.stderr)
        return 1
    print(f"build_shaders: done; summary in {paths['summary']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
