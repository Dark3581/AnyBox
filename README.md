# AnyBox

Tool for porting GDK console executables to Windows PCs, by relinking them against reimplemented system libraries.

No emulation and no separate runtime process. A GDK title is already an x86-64 PE program. The [relinker](core/relinker) points its imports at AnyBox's own DLLs ([core/libs/xdll](core/libs/xdll)) and switches the console subsystem to a Windows one. The converted program then runs as an ordinary Windows process.

[Usage](docs/user/USAGE.md) · [Build](docs/dev/BUILD.md) · [Architecture](docs/dev/ARCHITECTURE.md) · [Contributing](CONTRIBUTING.md) · [Conventions](docs/dev/CONVENTIONS.md) · [Technical debt](docs/dev/TechnicalDebt.md) · [Progress tracking](docs/dev/PROGRESS.md) · [Game testing](docs/dev/GAME_TESTING.md)

## Status

Early. The relinker, the library build, the run reports and the progress tracking work end to end on a test title. Most system functions are still stubs that stop the title with a report naming the function.

[![libraries](https://dark3581.github.io/AnyBox/badge-libraries.svg)](https://dark3581.github.io/AnyBox/progress.html)

[![progress map](https://dark3581.github.io/AnyBox/progress.svg)](https://dark3581.github.io/AnyBox/progress.html)

<sub>Each tile is a library, sized by its number of functions, and each square in it is one function: implemented (green), forwarded to Windows (blue), stub (grey) or known but not declared yet (dark). A dashed red tile is a DLL that titles import and no AnyBox library serves yet. `*` marks a library whose full export list is not known yet, so its total grows as titles are audited. Known functions come from the [export inventories](docs/inventory) and [title needs records](docs/needs).</sub>

The [progress page](https://dark3581.github.io/AnyBox/progress.html) lists every function as implemented, forwarded, stub or not declared, and every DLL with no library. It is regenerated on each push to `main` by [`tools/progress.py`](tools/progress.py); the [history](https://dark3581.github.io/AnyBox/history.html) shows how it grows.

| Library | Serves | State |
|---|---|---|
| [xgameruntime](core/libs/xdll/xgameruntime) | `xgameruntime.dll` | XGameRuntime init and options, all of XTaskQueue (composite queues, monitors and waiters included), XAsync, XThread and XGameSave implemented; XUser, XGameSaveFiles, most of XPackage and XSystem stubs |
| [d3d12_x](core/libs/xdll/d3d12_x) | `d3d12_x.dll`, `d3d12_xs.dll` | uses two back ends: root signature serialization is forwarded to the host's `d3d12.dll`, and everything the device does goes to Vulkan. Built on the Vulkan side: the device, shader store, compute and graphics pipeline state (render state, vertex input, cache first), resource barriers, texture images, views and samplers, bindless descriptors and the buffer window ([Graphics](#graphics-and-shaders)). Device creation still stops the title until its calls show the device's shape |
| [xaudio2](core/libs/xdll/xaudio2) | `xaudio2_9.dll` | forwarded to the host's XAudio2 |
| [gameinput](core/libs/xdll/gameinput) | `gameinput.dll` | forwarded to the host's GameInput |
| [kernelx](core/libs/xdll/kernelx) | `kernelx.dll` | Win32 subset forwarded to `kernel32` |
| [xmem](core/libs/xdll/xmem) | `xmem.dll` | `XMemAlloc`, `XMemFree`, `XMemVirtualQuery` and `XMemGetWorkingSetStatistics` implemented; the allocation attribute word is not decoded yet, so every block is plain read-write memory |

Unsupported or unexpected states stop the process with a [run report](docs/run-report.schema.json) (`anybox-report.json`) that names the library and function. Nothing returns a made-up success.

## Graphics and shaders

Console shaders ship as native GPU code, not DXIL, so no desktop driver can take them as they are. AnyBox's graphics library ([d3d12_x](core/libs/xdll/d3d12_x)) therefore runs a title's D3D12.X device on Vulkan, with the title's shaders translated to SPIR-V ahead of time by an external translator. The parts of D3D12 that need no GPU, root signature serialization and deserialization, go to the host's own `d3d12.dll`. The [design is on #4](https://github.com/Dark3581/AnyBox/issues/4) and the pieces are in [Architecture](docs/dev/ARCHITECTURE.md#graphics-d3d12_x).

Built and tested on a GPU without the title's executable (each with its own probe and tests):

- **Pipeline state**: compute and graphics descriptions resolve their shaders through the store by the hash of each shader container. Blend, rasterizer, depth-stencil, samples, topology and formats become Vulkan state, and the input layout is matched to the vertex shader's inputs. Each pipeline is created once and taken from the pipeline cache first; one that would need too much memory to compile in the title is refused, not compiled.
- **Resources**: D3D12 resource states become Vulkan barriers, texture descriptions become Vulkan images (typeless, depth and block-compressed formats included), and view and sampler descriptions become Vulkan image views and samplers.
- **Shader resources**: the descriptor hash table and bindless set the translated shaders look their images and samplers up in, and the buffer window below 4 GB their buffer addresses point into.

Still needing the executable: the device object and its methods, command lists that record into Vulkan, descriptor heaps and the console descriptor words written into them, the root signature to shader register mapping, mesh pipelines and presentation.

The shader set is built on your PC from your own copy, into the build folder, like the rest of the project. It is never committed or shared. With a checkout of the translator:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON ^
  -DANYBOX_GAME_PACKAGE=<package folder> -DANYBOX_SHADER_TRANSLATOR=<translator checkout>
cmake --build build
cmake --build build --target shaders            # translate and build the shader store (about an hour)
cmake --build build --target shader_pipelines   # create every pipeline on your GPU (hours; resumes if stopped)
ctest --test-dir build -L shaders --output-on-failure
```

The settings (worker count, wave64 emulation for 32-wide GPUs, pipeline chunk size, the size limit for modules too large for your PC's memory) and what each test checks are in [Building shaders with the project](docs/dev/GAME_TESTING.md#building-shaders-with-the-project).

### How far along

Measured on one title's package (26,503 shaders) with an RTX 3070 Ti and 32 GB of RAM:

| Step | State |
|---|---|
| Translate to SPIR-V | ✅ 26,503 of 26,503 (72 minutes) |
| Shader store | ✅ 25,889 modules (614 containers are byte-identical copies), none missing, invalid or conflicting; the C++ store finds all 26,503 containers |
| GPU support | ✅ the GPU supports every feature the modules need |
| Title layout | ✅ every module fits the one descriptor layout and the 144-byte push constants |
| Compute sample through AnyBox's own pipeline code | ✅ 60 of 60, no validation errors |
| Bindless descriptors and buffer access on the GPU | ✅ 3,701 of 3,701 descriptor lookups; buffer windows below 4 GB of up to 2 GiB |
| The title's shaders through AnyBox's pipeline state code | ✅ 12 compute shaders and 8 vertex shaders with vertex inputs become pipelines from their own containers; the largest compute shader is refused in 0.3 s instead of a 75 GB compile; with the offline cache, the pipelines come back without compiling |
| Every pipeline created by the driver | 🔄 in progress, in chunks of 500: past 15,500 of 26,503 with no failure. Two compute modules of 43 MiB each need about 75 GB of memory to compile, more than this PC can commit, so they are skipped by size here |
| Shaders running in a title | ❌ needs the title's executable: device creation, command lists, root signatures and presentation follow from its calls |

What is not proven yet: that any translated shader computes the right values inside the title, and how the title binds its resources at run time. Both need the executable.

## Compatibility

Per-title status lives in the **Game bring-up** discussions, one thread per title ID. Start a thread with the [bring-up form](.github/DISCUSSION_TEMPLATE/game-bring-up.yml) and paste the line from `python tools/run_report_summary.py anybox-report.json`.

## Where to start contributing

1. Pick the top item on the [needs page](https://dark3581.github.io/AnyBox/needs.html), which ranks what titles need and AnyBox lacks. Or audit a title you own with `python tools/import_audit.py <package> --needs docs/needs/<TitleId>.json` and contribute the record so it counts.
2. Read [CONTRIBUTING.md](CONTRIBUTING.md) and the [XTaskQueue implementation](core/libs/xdll/xgameruntime/TaskQueue.cpp) as a worked example.
3. Open a pull request with the [template](.github/pull_request_template.md). CI runs the conventions check and posts the progress change.

## Disclaimer

This project is intended for interoperability, research, preservation and compatibility. It does not include, distribute or require copyrighted software, firmware, cryptographic keys or proprietary libraries, and it does not decrypt or unpack protected packages. It works only on executables the user has already obtained in plain form, in accordance with applicable laws and license terms.

AnyBox is an independent project. It is not affiliated with, authorized or endorsed by any platform holder. All trademarks belong to their respective owners.

AnyBox is built to be like [AnyPS5](https://github.com/boykopovar/AnyPS5), for GDK titles.

## License

GNU General Public License version 2 only. See [LICENSE](LICENSE).
