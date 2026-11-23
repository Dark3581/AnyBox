# Game testing

<p align="center">
  <a href="../../README.md">Home</a> &nbsp;·&nbsp;
  <a href="../user/USAGE.md">Usage</a> &nbsp;·&nbsp;
  <a href="BUILD.md">Build</a> &nbsp;·&nbsp;
  <a href="ARCHITECTURE.md">Architecture</a> &nbsp;·&nbsp;
  <b>Game testing</b> &nbsp;·&nbsp;
  <a href="../../CONTRIBUTING.md">Contributing</a> &nbsp;·&nbsp;
  <a href="CONVENTIONS.md">Conventions</a> &nbsp;·&nbsp;
  <a href="TechnicalDebt.md">Technical debt</a> &nbsp;·&nbsp;
  <a href="PROGRESS.md">Progress</a>
</p>

Every run is useful, including one that stops at once: the run report names the next function to implement. This page asks for the same information from every run, so reports can be compared.

## Before a run

- Build `main` (or the pull request you are testing) in a clean tree: `cmake --build build`.
- Audit the title first: `python tools/import_audit.py <package> --json audit.json`. Keep the summary line for the report.
- Convert with `--registry` and every folder of title DLLs as `--module-dir` ([USAGE](../user/USAGE.md)).
- Check that the converted DLLs load: `python tools/module_check.py <out> --build-dir build`. Each DLL with a `.registry.json` beside it (every DLL except AnyBox's `ab_*.dll` when none has one) is loaded by `anybox-module-probe.exe` in its own process, which runs its `DllMain`. A DLL that fails names the Win32 error and, when `DllMain` raised one, the exception code with module and offset; one that calls a stub names the AnyBox function. It needs no executable, so it also works on a package whose executable is missing.
- Check the GPU against the title's shader store ([Shader store](#shader-store)): `python tools/gpu_report.py --build-dir build --store <out>\shaders`. It lists each Vulkan device and every feature, push constant size, subgroup operation per stage or 64-wide subgroup the store's modules need and the device lacks. Without `--store` it lists the devices only.
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
| Caller | `anybox-report.json` `stop.caller`, log line `called from` | where in the title the stop was reached: the first frame outside AnyBox and Windows, as `module+offset` (null when the title is not on the stack, for example on an AnyBox worker thread) |
| Frames | `anybox-report.json` `stop.frames`, log lines `#00`, `#01`, ... | up to 32 frames of the stopping or crashing thread |
| Run-time loads | log lines `loader: LoadLibraryExW(xgameruntime.dll) from game.exe+0x... loads ab_xgameruntime.dll` | a module loaded a DLL AnyBox serves by name at run time, and got AnyBox's library instead of whatever the search would have found |
| Libraries | `anybox-report.json` `libraries` | which AnyBox DLLs the title loaded |
| Log | stderr or `ANYBOX_LOG_FILE` | `[anybox] not implemented: lib!func (file:line)` |
| Loader errors | message box at start-up | "The procedure entry point X could not be located in Y": a forwarded or host export the desktop lacks; report it as a library gap |

## Debugging a stop

A stop names the AnyBox function and, under `caller`, the place in the title that reached it, such as `called from game.exe+0x31013`. The relinker leaves the title's sections where they were, so that offset is the same RVA in the original executable: in Ghidra or IDA, go to the image base plus the offset (`0x140000000 + 0x31013` for the usual base). Frames are return addresses, so the offset is the instruction just after the call; the call is the instruction before it. A crash lists its frames the same way, starting at the faulting instruction.

To look at the title's state at the stop, run it under a debugger (x64dbg, WinDbg or Visual Studio). With a debugger attached, a stop breaks into it before the process exits, with the title's call on the stack; set `ANYBOX_BREAK=0` to turn that off. When something else starts the title, set `ANYBOX_WAIT_DEBUGGER=<seconds>`: AnyBox logs its process ID, waits up to that long for a debugger to attach, and breaks before the title's entry point runs. The debugger also receives its own attach break, so continue past both.

| Variable | Effect |
|---|---|
| `ANYBOX_LOG_FILE=<path>` | append the log to a file as well as stderr |
| `ANYBOX_TRACE=<library>[,<library>...]` or `all` | log each traced call of those libraries |
| `ANYBOX_REPORT=<path>` | write the run report there instead of beside the executable |
| `ANYBOX_NO_REPORT=1` | write no run report |
| `ANYBOX_BREAK=0` | do not break into an attached debugger at a stop |
| `ANYBOX_WAIT_DEBUGGER=<seconds>` | wait for a debugger at start-up, then break before the title starts |
| `ANYBOX_CONFIG=<path>` | read settings from that file instead of `anybox.cfg` beside the executable |
| `ANYBOX_SET=<key>=<value>[;...]` | set or override settings for one run |

## Settings for values found during bring-up

Some values AnyBox needs depend on the console build a title was made for and are not known until someone finds them in that title; until then the feature that needs one stops with a message naming the missing value. Such values are settings, so trying one needs no rebuild. Put them in `anybox.cfg` beside the executable, one `key = value` per line, with a `#` comment saying where each came from:

```
# <title> <version>: GetDescriptorHandleIncrementSize result, seen at game.exe+0x1a2b30
d3d12_x.descriptor_size.shader_resource = 32
```

Each value a library reads is logged with its source (`setting ... = 32 (from anybox.cfg:2)`), and malformed or repeated lines are logged too. `ANYBOX_SET` overrides the file for one run. A setting describes one title's build: report what you found in the title's bring-up thread, and a value only becomes a default in AnyBox once it is confirmed and its source recorded.

| Key | Meaning |
|---|---|
| `d3d12_x.descriptor_size.shader_resource` | descriptor handle increment of CBV, SRV and UAV heaps, in bytes |
| `d3d12_x.descriptor_size.sampler` | descriptor handle increment of sampler heaps |
| `d3d12_x.descriptor_size.render_target` | descriptor handle increment of render target heaps |
| `d3d12_x.descriptor_size.depth_stencil` | descriptor handle increment of depth-stencil heaps |
| `d3d12_x.vulkan_device` | part of the name of the GPU to use, case-insensitive, when several are installed (`python tools/gpu_report.py` lists them); without it the best native GPU is used, and a layered driver such as Dozen only when nothing else can run the title |

## Static analysis with Ghidra

When a title stops in a stub, the next question is what it passes to that function. [`tools/ghidra/ImportCallArgs.java`](../../tools/ghidra/ImportCallArgs.java) lists every call a title's executable or DLL makes into one imported DLL, and the instructions that set `RCX`, `RDX`, `R8` and `R9` before each call. Run it headless on the original (unconverted) file, with Ghidra 11 or later:

```
analyzeHeadless <project dir> anybox -import <package>\Bink2Scarlett.dll -scriptPath tools\ghidra -postScript ImportCallArgs.java xmem.dll calls.txt -overwrite
```

The output names each call site, its function and the argument setup, for example `RDX <- MOV EDX,0x5e50000` for the attributes passed to `XMemAlloc`. Use it to find which inputs a title exercises and to check a signature against real callers. Values seen in one title describe that title only: implement the function for every input, and put the call sites in the pull request, not in the repository.

## GDK runtime method tables

GDK modules do not import the runtime's functions: they load `xgameruntime.dll` at run time and call method tables that `QueryApiImpl` returns, so the run report names an interface and slot instead of a function. [`tools/api_impl_scan.py`](../../tools/api_impl_scan.py) lists, for one executable or DLL, every thunk the module contains (the GUID pair it passes to `QueryApiImpl` and the method slot it calls), and for the thunks the module's own code calls, how many call sites they have and which argument registers and stack slots each passes:

```
python tools/api_impl_scan.py <package>\game.exe --json game-apis.json
```

It needs GNU `objdump` on `PATH` (MinGW's works) or given with `--objdump`. A slot is only mapped in `ApiImpl.cpp` when call sites show what it is: argument shape, how the result is used, and an open-source caller or documented order that names it. Unmapped slots stop and name themselves, so a run and this listing together say which slot to measure next.

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

## Shader store

A title's native shaders cannot be handed to a desktop driver, so AnyBox looks each one up in a shader store: SPIR-V modules translated ahead of time by a separate tool, keyed by the SHA-256 of the whole DXBC container (the container's own `HASH` part is not unique enough: different containers share it). AnyBox does not contain a translator. Build the store from a translated shader set laid out as a folder of carved `.dxbc` containers with an `index.json` list (`name`, `file`) and a folder of `<name>.spv` modules with an `index.json` map from name to details that include `stage`:

```
python tools/shader_store.py build <carved folder> <translated folder> <out>\shaders
python tools/shader_store.py lookup <out>\shaders <container.dxbc>
```

`build` writes `modules.txt` (one `<sha256> <stage> <subgroup> <path>` line per module, tab-separated; the subgroup size a pipeline must require is `32`, `64` or `0` for any; paths are relative to the store), `inputs.txt` (one `<sha256> <location> <semantic> <index> <type> <mask>` line per vertex input of a vertex shader: the input register the translated module reads at that location, the semantic the input layout names it by, and its component type) and `modules.json` (the translator's details per module). Byte-identical containers share a module; a hash whose copies translate to different modules or stages is left out and counted. `d3d12_x` loads `modules.txt` from `ANYBOX_SHADER_STORE`, or from `shaders` beside the executable. The store is built from your own copy and stays on your machine: never commit it or attach it to an issue.

## Building shaders with the project

The shader set can be built and tested like the rest of the project, from your own package and into the build folder, so nothing of it is ever committed. Point CMake at the package and at a checkout of the external translator (a folder holding `make_game.py`, which takes `--game`, `--work`, `--stages shaders` and `--wave64-emulate` and leaves the carved containers in `<work>/shaders/dxbc` and the modules in `<work>/shaders/spirv`, the layout `shader_store.py build` reads):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON ^
  -DANYBOX_GAME_PACKAGE=<package folder> -DANYBOX_SHADER_TRANSLATOR=<translator checkout>
cmake --build build
cmake --build build --target shaders
ctest --test-dir build -L shaders --output-on-failure
```

| Setting | Meaning |
|---|---|
| `ANYBOX_GAME_PACKAGE`, `ANYBOX_SHADER_TRANSLATOR` | Both or neither. Without them none of this is configured, and the normal build and tests are unchanged. |
| `ANYBOX_PIPELINE_CHECK` | The pipeline checker executable. Default: the translator's `runtime/build/vk_pipeline_check.exe`. |
| `ANYBOX_SHADER_JOBS` | Workers for the translator and the pipeline check. `0` (default) leaves the choice to them. |
| `ANYBOX_WAVE64_EMULATE` | The translator's wave64 emulation mode, `auto` (default), `on` or `off`. 32-wide GPUs need it on. |
| `ANYBOX_PIPELINE_CHUNK` | Index entries per pipeline checker process, `500` by default. The GPU driver's compiler keeps its memory until the process exits, so one process for the whole set can run out of memory hours in. `0` runs one process. |
| `ANYBOX_PIPELINE_MAX_BYTES` | Modules larger than this many bytes are reported as `BIG SKIP` instead of compiled. `0` (default) compiles all. Use it when a few huge modules need more memory than this PC can commit: the rest of their chunk is still checked, and the check reports the skipped modules by name and does not pass. |

The translator's own requirements (its disassembler, the Vulkan SDK's `spirv-as` and `spirv-val`) come from your environment, as when you run it by hand.

| Target | What it does |
|---|---|
| `shaders` | Runs the translator's shader stage into `build/shaders/work`, then builds the store in `build/shaders/store`. The translator skips the stages whose output is still current, so a second run is quick. |
| `shader_store` | Rebuilds the store only. |
| `shader_pipelines` | Creates every pipeline on this PC's GPU from `build/shaders/work/shaders/spirv`, with its cache in `build/shaders/pipeline_cache.bin`. It runs the checker once per chunk of `ANYBOX_PIPELINE_CHUNK` entries, and every chunk seeds from and saves to that cache. This takes hours and needs a lot of memory for the largest modules; run it when the translation changes, not on every build. A run that stops or fails part way picks up where it left off: chunks that passed with the same checker executable are reused, and the cache makes the pipelines of an interrupted chunk quick to create again. Pass `--recheck` to `tools/build_shaders.py` to check every chunk again. |

Each step writes `build/shaders/build.json`: the translator's revision and whether it had local changes, the counts of carved and translated shaders, the store's counts, and the pipeline totals with the name of every failed or skipped pipeline. For each chunk that failed it also records the checker's exit code and the last compile the checker named as still running, which is the likely culprit when the checker crashed. Logs go to `build/shaders/translate.log` and one log per chunk in `build/shaders/pipelines/`. A new translation drops the store and pipeline results from `build.json`, so the tests never pass on results from an older translation.

| Test (label `shaders`) | Passes when |
|---|---|
| `shaders_store` | Every carved shader has a translation, the store has no missing, invalid or conflicting entries, and the C++ store (`d3d12_x`) finds every carved container with the stage the translation gives. |
| `shaders_gpu` | At least one Vulkan device supports what every stored module needs (`tools/gpu_report.py`). Skipped without Vulkan. |
| `shaders_layout` | Every stored module's descriptor bindings (read from its SPIR-V by `tools/spirv_layout.py`) and push constant size fit the title layout `d3d12_x` creates pipelines with (`anybox-pipeline-probe layout`). |
| `shaders_compute` | An evenly spaced sample of 60 compute modules up to 1 MiB compiles through `d3d12_x`'s own device and pipeline code, under the Khronos validation layer when it is installed (any validation error fails the test). Skipped without a usable Vulkan device. |
| `shaders_pso` | 12 compute shaders up to 1 MiB and the largest compute shader go from their carved containers through the store and `d3d12_x`'s pipeline state path, with a 1 MiB run-time compile limit. With an empty cache the small ones are created and the largest is refused rather than compiled (or found in the driver's own cache). With `build/shaders/pipeline_cache.bin` every one must come from the cache once `shaders_pipelines` passes. Then 8 vertex shaders that read vertex inputs become graphics pipelines through the store, an input layout built from their signature and the render state path, with a test pixel shader. Skipped without a usable Vulkan device. |
| `shaders_pipelines` | The last `shader_pipelines` run covered every entry, every chunk completed, and no module or pipeline failed. Skipped until it has run. |

`build.json` and the logs name shaders: like the store, they stay on your machine.
