import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

import helpers

import build_shaders

FAKE_TRANSLATOR = r'''
import argparse, json, os, struct, sys
from pathlib import Path
parser = argparse.ArgumentParser()
for name in ("--game", "--work", "--out", "--stages", "--wave64-emulate", "--jobs"):
    parser.add_argument(name)
args = parser.parse_args()
assert args.stages == "shaders", args.stages
if os.environ.get("FAKE_TRANSLATOR_EXIT"):
    print("fake translator failing")
    sys.exit(int(os.environ["FAKE_TRANSLATOR_EXIT"]))
carved = Path(args.work) / "shaders" / "dxbc"
translated = Path(args.work) / "shaders" / "spirv"
carved.mkdir(parents=True, exist_ok=True)
translated.mkdir(parents=True, exist_ok=True)
shaders = [("ps_a", 1, "ps"), ("cs_b", 2, "cs"), ("ps_a_again", 1, "ps")]
for name, fill, stage in shaders:
    data = bytearray(bytes([fill]) * 48)
    data[0:4] = b"DXBC"
    struct.pack_into("<I", data, 24, 48)
    (carved / f"{name}.dxbc").write_bytes(bytes(data))
    (translated / f"{name}.spv").write_bytes(b"\x03\x02\x23\x07" + bytes([fill]) * 8)
(carved / "index.json").write_text(json.dumps([{"name": n, "file": f"{n}.dxbc"} for n, _, _ in shaders]), encoding="utf-8")
skip = os.environ.get("FAKE_TRANSLATOR_SKIP")
(translated / "index.json").write_text(json.dumps({n: {"stage": s} for n, _, s in shaders if n != skip}), encoding="utf-8")
(translated / "wave64_emulate.txt").write_text(args.wave64_emulate + "\n", encoding="utf-8")
print(f"translated {len(shaders)} shaders for {args.game} jobs={args.jobs}")
'''

FAKE_CHECKER = r'''
import os, sys
args = sys.argv[1:]
mode = args[0]
start, count = int(args[args.index("--start") + 1]), int(args[args.index("--count") + 1])
if os.environ.get("FAKE_CHECKER_RUNS"):
    with open(os.environ["FAKE_CHECKER_RUNS"], "a", encoding="utf-8") as handle:
        handle.write(f"{start}\n")
print(f"checking {count} shaders [{start}, {start + count}) (stage=all) on 2 threads")
if mode == "crash" and start > 0:
    print("  still compiling big_cs after 62s")
    sys.exit(7)
ps = count
if mode == "big" and start == 0:
    ps -= 1
    print("BIG SKIP huge_cs (70000000 bytes, limit 41943040)")
if mode == "fail" and start == 0:
    ps -= 1
    print("PIPELINE FAIL cs_b (cs w32, -3)")
    print("cs: 1 modules ok, 0 failed")
    print("cs pipelines: 0 ok, 1 failed, 0 skipped")
print(f"ps: {ps} modules ok, 0 failed")
print(f"ps pipelines: {ps} ok, 0 failed, 0 skipped")
print("wall time: 1.0s on 2 threads")
'''

SUMMARY = """checking 26503 shaders [0, 26503) (stage=all) on 6 threads
SLOW CS heavy (w32, 61.0s)
PIPELINE FAIL broken (cs w32, -3)
BIG SKIP huge (70000000 bytes, limit 41943040)
SKIPPED odd (no stage)
cs: 9000 modules ok, 1 failed
cs pipelines: 8998 ok, 1 failed, 1 skipped
  cs wave32: 8998 ok, 1 failed; wave64: 0 ok, 0 failed
ps: 17500 modules ok, 0 failed
ps pipelines: 17500 ok, 0 failed, 0 skipped
wall time: 5.0s on 6 threads
"""


def quiet(function, *args, **kwargs):
    output, errors = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
        code = function(*args, **kwargs)
    return code, output.getvalue(), errors.getvalue()


class ParseTest(unittest.TestCase):
    def test_summary_counts_and_problems(self):
        result = build_shaders.parse_pipelines(SUMMARY)
        self.assertTrue(result["complete"])
        self.assertEqual(result["stages"]["cs"], {"modules_ok": 9000, "modules_failed": 1, "pipelines_ok": 8998, "pipelines_failed": 1, "pipelines_skipped": 1})
        self.assertEqual(result["totals"], {"modules_ok": 26500, "modules_failed": 1, "pipelines_ok": 26498, "pipelines_failed": 1, "pipelines_skipped": 1})
        self.assertEqual(result["problems"], [{"kind": "pipeline_fail", "name": "broken"}, {"kind": "big_skip", "name": "huge"}, {"kind": "skipped", "name": "odd"}])

    def test_a_crashed_run_is_incomplete(self):
        result = build_shaders.parse_pipelines("checking 10 shaders\n  pipelines 250 (3s elapsed)\nNVVM compilation failed: 3\n")
        self.assertFalse(result["complete"])
        result["exit_code"] = 139
        self.assertFalse(build_shaders.pipelines_passed(result))

    def test_pass_needs_a_clean_complete_run(self):
        clean = build_shaders.parse_pipelines("ps: 2 modules ok, 0 failed\nps pipelines: 2 ok, 0 failed, 0 skipped\nwall time: 1s on 1 threads\n")
        clean["exit_code"] = 0
        self.assertTrue(build_shaders.pipelines_passed(clean))
        self.assertFalse(build_shaders.pipelines_passed(dict(clean, exit_code=1)))
        failed = build_shaders.parse_pipelines(SUMMARY)
        failed["exit_code"] = 0
        self.assertFalse(build_shaders.pipelines_passed(failed))
        skipped = build_shaders.parse_pipelines("SKIPPED odd (no stage)\nps: 1 modules ok, 0 failed\nwall time: 1s on 1 threads\n")
        skipped["exit_code"] = 0
        self.assertTrue(build_shaders.pipelines_passed(skipped))

    def test_steps(self):
        self.assertEqual(build_shaders.parse_steps("store, translate"), ["translate", "store"])
        with self.assertRaises(build_shaders.BuildError):
            build_shaders.parse_steps("translate,link")
        with self.assertRaises(build_shaders.BuildError):
            build_shaders.parse_steps(" , ")

    def test_commands(self):
        paths = build_shaders.layout(Path("out"))
        command = build_shaders.translator_command(Path("tr"), Path("pkg"), paths, 4, "on")
        self.assertEqual(command[1:3], ["-I", str(Path("tr") / "make_game.py")])
        self.assertEqual(command[command.index("--stages") + 1], "shaders")
        self.assertEqual(command[command.index("--wave64-emulate") + 1], "on")
        self.assertEqual(command[-2:], ["--jobs", "4"])
        self.assertNotIn("--jobs", build_shaders.translator_command(Path("tr"), Path("pkg"), paths, 0, "auto"))
        self.assertEqual(build_shaders.pipeline_command(Path("check.exe"), paths, 0, 500, 250),
                         ["check.exe", str(paths["translated"]), str(paths["cache"]), "--start", "500", "--count", "250"])
        self.assertEqual(build_shaders.pipeline_command(Path("check.exe"), paths, 6, 0, 1)[-2:], ["--jobs", "6"])
        self.assertEqual(build_shaders.pipeline_command(Path("check.exe"), paths, 0, 0, 1, 41943040)[-2:], ["--max-bytes", "41943040"])

    def test_chunk_ranges(self):
        self.assertEqual(build_shaders.chunk_ranges(1201, 500), [(0, 500), (500, 500), (1000, 201)])
        self.assertEqual(build_shaders.chunk_ranges(1000, 500), [(0, 500), (500, 500)])
        self.assertEqual(build_shaders.chunk_ranges(1201, 0), [(0, 1201)])
        self.assertEqual(build_shaders.chunk_ranges(3, 500), [(0, 3)])
        self.assertEqual(build_shaders.chunk_ranges(0, 500), [])

    def test_a_crash_names_the_last_compile(self):
        result = build_shaders.parse_pipelines("  still compiling small after 61s\n  still compiling huge_cs after 422s\n")
        self.assertEqual(result["last_compiling"], "huge_cs")
        self.assertIsNone(build_shaders.parse_pipelines(SUMMARY)["last_compiling"])

    def test_combined_chunks(self):
        def chunk(start, count, text, code=0):
            return {"start": start, "count": count, **build_shaders.parse_pipelines(text), "exit_code": code}
        good = "ps: 2 modules ok, 0 failed\nps pipelines: 2 ok, 0 failed, 0 skipped\nwall time: 1s on 1 threads\n"
        crashed = "  still compiling huge_cs after 422s\n"
        result = build_shaders.combine_chunks([chunk(0, 2, good), chunk(2, 2, good)], 4)
        self.assertTrue(result["complete"])
        self.assertEqual(result["totals"]["pipelines_ok"], 4)
        self.assertEqual(result["failed_chunks"], [])
        self.assertTrue(build_shaders.pipelines_passed(result))
        self.assertFalse(build_shaders.combine_chunks([chunk(0, 2, good)], 4)["complete"])
        self.assertFalse(build_shaders.combine_chunks([chunk(0, 2, good), chunk(3, 1, good)], 4)["complete"])
        result = build_shaders.combine_chunks([chunk(0, 2, good), chunk(2, 2, crashed, 3221225477)], 4)
        self.assertFalse(result["complete"])
        self.assertEqual(result["exit_code"], 3221225477)
        self.assertEqual(result["failed_chunks"], [{"start": 2, "count": 2, "exit_code": 3221225477, "last_compiling": "huge_cs"}])
        self.assertFalse(build_shaders.pipelines_passed(result))


class BuildTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        root = Path(self.directory.name)
        self.translator, self.package, self.out = root / "translator", root / "package", root / "out"
        self.translator.mkdir()
        self.package.mkdir()
        (self.translator / "make_game.py").write_text(FAKE_TRANSLATOR, encoding="utf-8")
        self.checker = root / "checker.py"
        self.checker.write_text(FAKE_CHECKER, encoding="utf-8")
        self.original_pipeline_command = build_shaders.pipeline_command

    def tearDown(self):
        build_shaders.pipeline_command = self.original_pipeline_command
        self.directory.cleanup()

    def build(self, *extra):
        return quiet(build_shaders.main, ["--package", str(self.package), "--translator", str(self.translator), "--out", str(self.out), *extra])

    def summary(self):
        return json.loads((self.out / "build.json").read_text(encoding="utf-8"))

    def test_translate_and_store(self):
        code, output, errors = self.build("--jobs", "3", "--wave64-emulate", "on")
        self.assertEqual(code, 0, errors)
        self.assertIn("jobs=3", output)
        summary = self.summary()
        self.assertEqual(summary["format"], "anybox-shader-build")
        self.assertEqual(summary["translation"], {"exit_code": 0, "carved": 3, "translated": 3, "wave64_emulate": "on"})
        self.assertEqual(summary["store"], {"modules": 2, "duplicates": 1, "missing": 0, "invalid": 0, "conflicts": 0})
        self.assertNotIn("pipelines", summary)
        self.assertEqual(len((self.out / "store" / "modules.txt").read_text(encoding="utf-8").splitlines()), 2)
        self.assertIn("translated 3 shaders", (self.out / "translate.log").read_text(encoding="utf-8"))

    def test_translator_failure_is_reported(self):
        os.environ["FAKE_TRANSLATOR_EXIT"] = "5"
        try:
            code, _, errors = self.build()
        finally:
            del os.environ["FAKE_TRANSLATOR_EXIT"]
        self.assertEqual(code, 1)
        self.assertIn("exited with 5", errors)
        self.assertEqual(self.summary()["translation"], {"exit_code": 5})
        self.assertFalse((self.out / "store").exists())

    def test_incomplete_store_fails(self):
        os.environ["FAKE_TRANSLATOR_SKIP"] = "cs_b"
        try:
            code, _, errors = self.build()
        finally:
            del os.environ["FAKE_TRANSLATOR_SKIP"]
        self.assertEqual(code, 1)
        self.assertIn("store is incomplete", errors)
        self.assertEqual(self.summary()["store"]["missing"], 1)

    def use_checker(self, mode):
        build_shaders.pipeline_command = lambda checker, paths, jobs, start, count, max_bytes=0: [
            sys.executable, str(self.checker), mode, "--start", str(start), "--count", str(count)]

    def check(self, *extra):
        return self.build("--steps", "pipelines", "--pipeline-check", str(self.checker), *extra)

    def runs(self, log):
        return [int(line) for line in log.read_text(encoding="utf-8").split()] if log.exists() else []

    def test_pipelines_step(self):
        self.assertEqual(self.build()[0], 0)
        self.use_checker("pass")
        code, output, errors = self.check()
        self.assertEqual(code, 0, errors)
        summary = self.summary()
        self.assertEqual(summary["store"]["modules"], 2)
        self.assertEqual(summary["pipelines"]["totals"]["pipelines_ok"], 3)
        self.assertEqual(summary["pipelines"]["exit_code"], 0)
        self.assertEqual(len(summary["pipelines"]["chunks"]), 1)
        self.use_checker("fail")
        code, _, errors = self.check("--recheck")
        self.assertEqual(code, 1)
        self.assertIn("did not pass", errors)
        self.assertEqual(self.summary()["pipelines"]["problems"], [{"kind": "pipeline_fail", "name": "cs_b"}])

    def test_size_skips_leave_the_check_incomplete_not_failed(self):
        self.assertEqual(self.build()[0], 0)
        self.use_checker("big")
        code, output, errors = self.check()
        self.assertEqual(code, 1)
        self.assertIn("is incomplete: 1 modules over --pipeline-max-bytes were not checked", errors)
        self.assertNotIn("did not pass", errors)
        self.assertIn("1 over the size limit; exit 0", output)
        self.assertNotIn("FAILED", output)
        self.assertIn("1 not checked (over the size limit)", output)
        self.assertIn("0 of 1 chunks failed", output)
        self.assertIn("not checked: huge_cs", output)
        self.assertEqual(self.summary()["pipelines"]["failed_chunks"], [])

    def test_chunks_resume_and_recheck(self):
        self.assertEqual(self.build()[0], 0)
        runs = Path(self.directory.name) / "runs.txt"
        os.environ["FAKE_CHECKER_RUNS"] = str(runs)
        try:
            self.use_checker("crash")
            code, output, errors = self.check("--pipeline-chunk", "2")
            self.assertEqual(code, 1)
            self.assertIn("last named compile big_cs", output)
            pipelines = self.summary()["pipelines"]
            self.assertEqual(self.runs(runs), [0, 2])
            self.assertFalse(pipelines["complete"])
            self.assertEqual(pipelines["failed_chunks"], [{"start": 2, "count": 1, "exit_code": 7, "last_compiling": "big_cs"}])
            self.assertTrue((self.out / "pipelines" / "000002-000003.log").is_file())

            self.use_checker("pass")
            code, output, errors = self.check("--pipeline-chunk", "2")
            self.assertEqual(code, 0, errors)
            self.assertEqual(self.runs(runs), [0, 2, 2])
            self.assertIn("[0, 2) passed with this checker before; reused", output)
            pipelines = self.summary()["pipelines"]
            self.assertTrue(pipelines["complete"])
            self.assertEqual(pipelines["totals"]["pipelines_ok"], 3)

            self.assertEqual(self.check("--pipeline-chunk", "2")[0], 0)
            self.assertEqual(self.runs(runs), [0, 2, 2])
            self.assertEqual(self.check("--pipeline-chunk", "2", "--recheck")[0], 0)
            self.assertEqual(self.runs(runs), [0, 2, 2, 0, 2])
            self.assertEqual(self.check("--pipeline-chunk", "0")[0], 0)
            self.assertEqual(self.runs(runs), [0, 2, 2, 0, 2, 0])
            self.checker.write_text(FAKE_CHECKER + "\n", encoding="utf-8")
            self.assertEqual(self.check("--pipeline-chunk", "0")[0], 0)
            self.assertEqual(self.runs(runs), [0, 2, 2, 0, 2, 0, 0])
        finally:
            del os.environ["FAKE_CHECKER_RUNS"]
        self.assertEqual(self.check("--pipeline-chunk", "-1")[0], 1)
        self.assertEqual(self.check("--pipeline-max-bytes", "-1")[0], 1)

    def test_a_new_translation_drops_old_results(self):
        self.assertEqual(self.build()[0], 0)
        self.use_checker("pass")
        self.assertEqual(self.check()[0], 0)
        self.assertIn("pipelines", self.summary())
        self.assertEqual(self.build("--steps", "translate")[0], 0)
        self.assertNotIn("pipelines", self.summary())
        self.assertNotIn("store", self.summary())

    def test_missing_inputs(self):
        code, _, errors = quiet(build_shaders.main, ["--package", str(self.package / "nowhere"), "--translator", str(self.translator), "--out", str(self.out)])
        self.assertEqual(code, 1)
        self.assertIn("is not a folder", errors)
        code, _, errors = quiet(build_shaders.main, ["--package", str(self.package), "--translator", str(self.package), "--out", str(self.out)])
        self.assertEqual(code, 1)
        self.assertIn("holds no make_game.py", errors)
        code, _, errors = self.build("--steps", "pipelines", "--pipeline-check", str(self.out / "none.exe"))
        self.assertEqual(code, 1)
        self.assertIn("not found", errors)


if __name__ == "__main__":
    unittest.main()
