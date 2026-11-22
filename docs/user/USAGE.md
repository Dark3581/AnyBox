# Usage

AnyBox converts a **decrypted** GDK title, a folder holding the main `.exe`, its DLLs and `MicrosoftGame.config`, into a program that runs on Windows. It does not decrypt or unpack protected packages.

## Convert

```
anybox-relinker --libs build/libs --module-dir . --registry <package>\game.exe <out>\game.exe
```

| Option | Meaning |
|---|---|
| `--libs <dir>` | copy the AnyBox DLLs the output needs (from `build/libs`) beside it |
| `--module-dir <dir>` | also convert every `.dll` under `<dir>` (relative to the package); repeat for each folder of title DLLs; `.` converts those beside the executable |
| `--registry` | write `<file>.registry.json` with every import and where it is routed |
| `--console` / `--gui` | choose the Windows subsystem (console images become GUI by default) |
| `--allow-unknown` | keep imports from DLLs nothing provides instead of failing |
| `--list-imports` | print the imports and their routes, write nothing |
| `--print-map` | print the DLL routing table |

Then copy the rest of the package (data folders and `MicrosoftGame.config`) into `<out>`. The relinker prints the expected layout.

## Convert and run in one step

```
python tools/launch_title.py <package> <out> --build-dir build
```

checks that the executable named in `MicrosoftGame.config` is a decrypted PE32+ x64 image, converts it with `--module-dir` for every folder holding DLLs, hard-links (or copies) the rest of the package into `<out>` except `.xvd` containers, starts the program with `ANYBOX_LOG_FILE=<out>\anybox.log`, and prints the run report summary. `--exe <path>` picks the executable when the config lists several, `--console` uses the console subsystem, and `--no-run` stops after converting.

## Before converting: audit

```
python tools/import_audit.py <package>
```

lists every import that is a stub, absent or unrouted, and exits with 1 if the title cannot start yet.

## Run

Start `<out>\game.exe`. When the title stops, `anybox-report.json` beside it says why:

| `stop.kind` | Exit code | Meaning |
|---|---|---|
| `none` | the title's | the process exited normally |
| `not-implemented` | `0xAB01` | the title called a stub; `library` and `function` name it |
| `unsupported` | `0xAB02` | an implemented function met a case whose behaviour is unknown; `message` says which |
| `crash` | the exception code | an unhandled exception; `exception` gives the module and offset |

`python tools/run_report_summary.py anybox-report.json` checks the report and prints one line to paste into the title's Game bring-up thread.

## Environment

| Variable | Effect |
|---|---|
| `ANYBOX_REPORT=<path>` | write the run report there instead of beside the executable |
| `ANYBOX_NO_REPORT=1` | write no run report |
| `ANYBOX_LOG_FILE=<path>` | append AnyBox's log lines to this file as well as stderr |
| `ANYBOX_TRACE=<library>[,<library>...]` | log every call into the listed AnyBox libraries (`xmem`, `d3d12_x`, ...; `all` for every library) with its arguments and result; case and spaces are ignored. Libraries that trace so far: `xmem`, `d3d12_x` |
| `ANYBOX_SHADER_STORE=<folder>` | load the shader store from this folder instead of `shaders` beside the executable ([GAME_TESTING](../dev/GAME_TESTING.md#shader-store)) |
