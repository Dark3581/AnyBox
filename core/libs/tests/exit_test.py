import argparse
import os
import shutil
import subprocess
from pathlib import Path


def run(probe, libs, work, scenario):
    folder = work / scenario
    folder.mkdir(parents=True)
    exe = folder / "lib_probe.exe"
    shutil.copy2(probe, exe)
    log = folder / "anybox.log"
    env = dict(os.environ, ANYBOX_NO_REPORT="1", ANYBOX_LOG_FILE=str(log))
    for name in ("ANYBOX_TRACE", "ANYBOX_CONFIG", "ANYBOX_SET", "ANYBOX_WAIT_DEBUGGER"):
        env.pop(name, None)
    result = subprocess.run([str(exe), str(libs), "exit", scenario], capture_output=True, text=True, env=env, timeout=60, cwd=folder)
    text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
    raw = folder / "anybox-crash-raw.txt"
    return result.returncode, text, raw.read_text(encoding="utf-8", errors="replace") if raw.exists() else ""


def expect(probe, libs, work, scenario, code, line, raw_line, caller):
    returned, text, raw = run(probe, libs, work, scenario)
    assert returned == code, (scenario, returned, text)
    assert "[anybox] process exits are logged once" in text, (scenario, text)
    assert line in text, (scenario, line, text)
    assert text.count("[anybox] process exit: ") == 1, (scenario, "logged once", text)
    assert raw_line in raw, (scenario, raw_line, raw)
    assert caller in text, (scenario, caller, text)
    return text


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--libs", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    shutil.rmtree(args.work, ignore_errors=True)
    args.work.mkdir(parents=True)

    called = ", called from lib_probe.exe+0x"
    outside = "first code outside AnyBox and the system: lib_probe.exe+0x"
    expect(args.probe, args.libs, args.work, "exit_process", 3,
           "[anybox] process exit: ExitProcess on this process with exit code 0x3 (3), on thread ", "exit ExitProcess code 0000000000000003", called)
    expect(args.probe, args.libs, args.work, "terminate_process", 4,
           "[anybox] process exit: TerminateProcess on this process with exit code 0x4 (4), on thread ", "exit TerminateProcess code 0000000000000004", called)
    expect(args.probe, args.libs, args.work, "exit_process_direct", 5,
           "[anybox] process exit: NtTerminateProcess on every other thread of this process (the first step of ending it) with exit code 0x5 (5)",
           "exit NtTerminateProcess code 0000000000000005", outside)
    expect(args.probe, args.libs, args.work, "terminate_process_direct", 6,
           "[anybox] process exit: NtTerminateProcess on this process with exit code 0x6 (6)", "exit NtTerminateProcess code 0000000000000006", outside)
    expect(args.probe, args.libs, args.work, "crt_exit", 7,
           "[anybox] process exit: exit on this process with exit code 0x7 (7)", "exit exit code 0000000000000007", called)
    text = expect(args.probe, args.libs, args.work, "main_thread_exit", 9,
                  "[anybox] process exit: ExitProcess on this process with exit code 0x9 (9)", "exit ExitProcess code 0000000000000009", called)
    assert "[anybox] the title's main thread " in text and " ended through ExitThread with exit code 0x8 (8), called from lib_probe.exe+0x" in text, text
    print("PASS exit")


if __name__ == "__main__":
    main()
