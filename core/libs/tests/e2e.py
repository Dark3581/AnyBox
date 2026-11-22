import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

CONFIG = """<?xml version="1.0" encoding="utf-8"?>
<Game configVersion="1">
  <Identity Name="AnyBox.FakeTitle" Publisher="CN=AnyBox" Version="1.0.2.0"/>
  <TitleId>1234abcd</TitleId>
  <ShellVisuals DefaultDisplayName="Fake Title"/>
</Game>
"""

def executable_list(family=None):
    attribute = "" if family is None else f' TargetDeviceFamily="{family}"'
    return f'  <ExecutableList>\n    <Executable Name="title.exe"{attribute}/>\n  </ExecutableList>\n'


DEVICES = {
    "device_scarlett": (executable_list("Scarlett"), 0x07),
    "device_durango": (executable_list("XboxOne"), 0x04),
    "device_pc": (executable_list("PC"), 0x01),
    "device_default": (executable_list(), 0x04),
    "device_none": (None, None),
    "device_noexecutable": ("", None),
    "device_unknown": (executable_list("Desktop"), None),
}

EXPECTED = {
    "taskqueue": (0, "none"),
    "taskqueue_composite": (0, "none"),
    "taskqueue_monitor": (0, "none"),
    "taskqueue_monitor_reentry": (0xAB02, "unsupported"),
    "taskqueue_waiter": (0, "none"),
    "taskqueue_terminate_reentry": (0xAB02, "unsupported"),
    "stub": (0xAB01, "not-implemented"),
    "crash": (0xC0000005, "crash"),
    "crash_chained": (0xC0000005, "crash"),
    "crash_recovered": (0, "none"),
    "device_scarlett": (0, "none"),
    "device_durango": (0, "none"),
    "device_pc": (0, "none"),
    "device_default": (0, "none"),
    "device_none": (0xAB02, "unsupported"),
    "device_noexecutable": (0xAB02, "unsupported"),
    "device_unknown": (0xAB02, "unsupported"),
    "console_id": (0, "none"),
    "sandbox_id": (0, "none"),
    "async": (0, "none"),
    "gamesave": (0, "none"),
    "gamesavefiles": (0, "none"),
    "gamesavefiles_mismatch": (0, "none"),
    "gamesavefiles_containers": (0xAB02, "unsupported"),
    "gamesave_folders": (0xAB02, "unsupported"),
    "gamesave_folders_async": (0xAB02, "unsupported"),
    "gamesave_empty_folder": (0, "none"),
    "async_null": (0xAB02, "unsupported"),
    "async_terminated": (0xAB02, "unsupported"),
    "async_begin_terminated": (0xAB02, "unsupported"),
    "startup": (0, "none"),
    "startup_options": (0xAB02, "unsupported"),
    "thread_assert": (0xAB02, "unsupported"),
}

STOPPED_IN = {
    "stub": "XUserAddAsync",
    "taskqueue_monitor_reentry": "XTaskQueueUnregisterMonitor",
    "taskqueue_terminate_reentry": "XTaskQueueTerminate",
    "device_none": "XSystemGetDeviceType",
    "device_noexecutable": "XSystemGetDeviceType",
    "device_unknown": "XSystemGetDeviceType",
    "async_null": "XAsyncGetStatus",
    "async_terminated": "XAsyncComplete",
    "async_begin_terminated": "XAsyncBegin",
    "startup_options": "XGameRuntimeInitializeWithOptions",
    "thread_assert": "XThreadAssertNotTimeSensitive",
    "gamesavefiles_containers": "XGameSaveFilesGetFolderWithUiAsync",
    "gamesave_folders": "XGameSaveInitializeProvider",
    "gamesave_folders_async": "XGameSaveInitializeProviderAsync",
}

STOP_MESSAGES = {
    "gamesavefiles_containers": "holds XGameSave containers",
    "gamesave_folders": "holds XGameSaveFiles files",
    "gamesave_folders_async": "holds XGameSaveFiles files",
}

SEEDS = {
    "gamesavefiles_containers": "keep.xgs",
    "gamesave_folders": "slot1/state.dat",
    "gamesave_folders_async": "slot1/deep/state.dat",
    "gamesave_empty_folder": "slot1/deep/",
}


def subsystem_offset(data):
    return struct.unpack_from("<I", data, 0x3C)[0] + 24 + 68


def set_console_subsystem(path):
    data = bytearray(path.read_bytes())
    struct.pack_into("<H", data, subsystem_offset(data), 14)
    path.write_bytes(bytes(data))


def subsystem(path):
    data = path.read_bytes()
    return struct.unpack_from("<H", data, subsystem_offset(data))[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--relinker", required=True, type=Path)
    parser.add_argument("--title", required=True, type=Path)
    parser.add_argument("--libs", required=True, type=Path)
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--scenario", required=True, choices=sorted(EXPECTED))
    args = parser.parse_args()

    package = args.work / "package"
    out = args.work / "out"
    shutil.rmtree(args.work, ignore_errors=True)
    package.mkdir(parents=True)
    title = package / "title.exe"
    shutil.copyfile(args.title, title)
    set_console_subsystem(title)
    executable, device = DEVICES.get(args.scenario, ("", None))
    config = None if executable is None else CONFIG.replace("  <ShellVisuals", executable + "  <ShellVisuals")
    if config is not None:
        (package / "MicrosoftGame.config").write_text(config, encoding="utf-8")

    converted = out / "title.exe"
    subprocess.run([str(args.relinker), "--console", "--registry", "--libs", str(args.libs), str(title), str(converted)], check=True)
    if config is not None:
        shutil.copyfile(package / "MicrosoftGame.config", out / "MicrosoftGame.config")

    assert subsystem(converted) == 3, "converted title is a console program"
    registry = json.loads((out / "title.exe.registry.json").read_text(encoding="utf-8"))
    routes = {module["dll"].lower(): module for module in registry["imports"]}
    assert routes["xgameruntime.dll"]["target"] == "ab_xgameruntime.dll", routes
    assert (out / "ab_xgameruntime.dll").exists() and (out / "ab_runtime.dll").exists(), "libraries are copied"

    argument = "device" if args.scenario in DEVICES else args.scenario
    local = args.work / "localappdata"
    local.mkdir()
    saves = local / "AnyBox" / "1234ABCD" / "XGameSave"
    if args.scenario in SEEDS:
        seed = saves / SEEDS[args.scenario]
        if SEEDS[args.scenario].endswith("/"):
            seed.mkdir(parents=True)
        else:
            seed.parent.mkdir(parents=True)
            seed.write_bytes(b"seed")
    env = dict(os.environ, LOCALAPPDATA=str(local))
    run = subprocess.run([str(converted), argument], cwd=out, env=env, capture_output=True, text=True, timeout=60)
    print(run.stdout)
    print(run.stderr, file=sys.stderr)
    code, kind = EXPECTED[args.scenario]
    assert (run.returncode & 0xFFFFFFFF) == code, f"exit code 0x{run.returncode & 0xFFFFFFFF:x}, expected 0x{code:x}"
    if args.scenario == "taskqueue":
        assert "task queue ok" in run.stdout
    if args.scenario == "taskqueue_composite":
        assert "composite ok" in run.stdout
    if args.scenario == "taskqueue_monitor":
        assert "monitor ok" in run.stdout
    if args.scenario == "taskqueue_waiter":
        assert "waiter ok" in run.stdout
    if args.scenario == "console_id":
        assert "console id ok" in run.stdout
    if args.scenario == "sandbox_id":
        assert "sandbox id ok" in run.stdout
    if args.scenario == "async":
        assert "async ok" in run.stdout
    if args.scenario == "startup":
        assert "startup ok" in run.stdout
    if args.scenario == "crash_chained":
        assert run.stdout.count("title filter") == 1, run.stdout
    if args.scenario == "crash_recovered":
        assert "recovered ok" in run.stdout, run.stdout
    if args.scenario == "gamesave":
        assert "gamesave ok" in run.stdout
        assert sorted(path.name for path in saves.iterdir()) == ["keep.xgs"], list(saves.iterdir())
    if args.scenario == "gamesavefiles":
        assert "gamesavefiles ok" in run.stdout
        assert sorted(path.name for path in saves.iterdir()) == ["loose.dat", "slot1", "slot2"], list(saves.iterdir())
        assert (saves / "slot1" / "state.dat").stat().st_size == 100
        assert (saves / "slot1" / "deep" / "more.dat").stat().st_size == 20
    if args.scenario == "gamesave_empty_folder":
        assert "gamesave empty folder ok" in run.stdout
    if args.scenario == "gamesavefiles_mismatch":
        assert "gamesavefiles mismatch ok" in run.stdout
        assert sorted(path.name for path in saves.iterdir()) == ["keep.xgs"], list(saves.iterdir())
    if device is not None:
        assert f"device type {device} packaged 0" in run.stdout, run.stdout

    report_path = out / "anybox-report.json"
    report = json.loads(report_path.read_text(encoding="utf-8"))
    assert report["format"] == "anybox-report" and report["version"] == 1
    assert report["stop"]["kind"] == kind, report["stop"]
    if config is None:
        assert report["game"]["titleId"] is None and report["game"]["name"] is None, report["game"]
    else:
        assert report["game"]["titleId"] == "1234ABCD", report["game"]
        assert report["game"]["name"] == "Fake Title" and report["game"]["version"] == "1.0.2.0", report["game"]
    assert "xgameruntime" in report["libraries"], report["libraries"]
    if kind in ("not-implemented", "unsupported"):
        assert report["stop"]["library"] == "xgameruntime" and report["stop"]["function"] == STOPPED_IN[args.scenario], report["stop"]
    if args.scenario in STOP_MESSAGES:
        assert STOP_MESSAGES[args.scenario] in report["stop"]["message"], report["stop"]
    if kind == "crash":
        assert report["stop"]["exception"]["code"] == "0xc0000005", report["stop"]
        assert report["stop"]["exception"]["module"] == "title.exe", report["stop"]

    schema = Path(__file__).resolve().parents[3] / "docs" / "run-report.schema.json"
    try:
        import jsonschema
    except ImportError:
        print("jsonschema not installed; schema check skipped")
    else:
        jsonschema.validate(report, json.loads(schema.read_text(encoding="utf-8")))
    print(f"PASS {args.scenario}")


if __name__ == "__main__":
    main()
