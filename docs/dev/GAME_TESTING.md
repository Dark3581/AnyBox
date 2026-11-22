# Game testing

Every run is useful, including one that stops at once: the run report names the next function to implement. This page asks for the same information from every run, so reports can be compared.

## Before a run

- Build `main` (or the pull request you are testing) in a clean tree: `cmake --build build`.
- Audit the title first: `python tools/import_audit.py <package> --json audit.json`. Keep the summary line for the report.
- Convert with `--registry` and every folder of title DLLs as `--module-dir` ([USAGE](../user/USAGE.md)).
- Run one title at a time.

## Run

```
set ANYBOX_LOG_FILE=%CD%\anybox.log
out\game.exe
echo exit=%ERRORLEVEL%
```

## Report

Post to the title's **Game bring-up** discussion (one thread per title ID; search first):

- the line from `python tools/run_report_summary.py out\anybox-report.json`;
- the import audit summary line;
- the AnyBox commit and any pull requests merged on top;
- OS, CPU, GPU and driver;
- what was on screen and how far it got.

Attach `anybox-report.json` and the log. Never attach game files, executables, keys or anything you do not have the right to share.

## What to read

| Check | Where | Meaning |
|---|---|---|
| Exit code | shell | `0xAB01` (43777) stub, `0xAB02` (43778) unsupported, `0xC0000005` access violation, `0xC0000135` a DLL is missing beside the executable |
| Stop | `anybox-report.json` `stop` | the library and function, or the crash module and offset |
| Libraries | `anybox-report.json` `libraries` | which AnyBox DLLs the title loaded |
| Log | stderr or `ANYBOX_LOG_FILE` | `[anybox] not implemented: lib!func (file:line)` |
| Loader errors | message box at start-up | "The procedure entry point X could not be located in Y": a forwarded or host export the desktop lacks; report it as a library gap |

## Static analysis with Ghidra

When a title stops in a stub, the next question is what it passes to that function. [`tools/ghidra/ImportCallArgs.java`](../../tools/ghidra/ImportCallArgs.java) lists every call a title's executable or DLL makes into one imported DLL, and the instructions that set `RCX`, `RDX`, `R8` and `R9` before each call. Run it headless on the original (unconverted) file, with Ghidra 11 or later:

```
analyzeHeadless <project dir> anybox -import <package>\Bink2Scarlett.dll -scriptPath tools\ghidra -postScript ImportCallArgs.java xmem.dll calls.txt -overwrite
```

The output names each call site, its function and the argument setup, for example `RDX <- MOV EDX,0x5e50000` for the attributes passed to `XMemAlloc`. Use it to find which inputs a title exercises and to check a signature against real callers. Values seen in one title describe that title only: implement the function for every input, and put the call sites in the pull request, not in the repository.

## Shader formats

The D3D12.X design ([#4](https://github.com/dark3581/AnyBox/issues/4)) depends on what console shaders contain. [`tools/shader_scan.py`](../../tools/shader_scan.py) finds every DXBC container in a package, loose or embedded in another file, and counts them by kind (`dxil`, `dxbc`, `mixed`, `rootsig`, `other`) and by part tag:

```
python tools/shader_scan.py <decrypted package folder> --json shaders.json
```

Part tags that are not standard DXBC parts point at console-specific data. Each container with a `DXIL` part also gets a `program` class, read from the bitcode slot of the DXIL program header:

| `program` | Meaning |
|---|---|
| `bitcode` | LLVM bitcode, so the shader is DXIL a desktop driver could take |
| `native` | the slot holds `NOBC` and the part carries an `XBCH` block: no DXIL, the GPU program is precompiled for the console |
| `stub` | `NOBC` without an `XBCH` block |
| `unknown` | the header does not parse |

In an RPF9 archive (file magic `9FPR`) the scan also reads entries stored as raw deflate, which the table of contents is not needed for: `<name> 00 <u8> <u32 stored> <u32> <u32 stored> <u32 size>` followed by `stored` bytes that inflate to a `size`-byte container. Those containers have `stored: deflate` and their `name`, and their `offset` is the entry header's. Shaders in other compressed archives are not found. Post the summary line in the issue; the JSON holds names, offsets and part tags only, never shader bytes.
