# Build

## Requirements

- CMake 3.20 or newer and Ninja.
- A C++20 compiler.
  - **Full build (Windows):** MinGW-w64 GCC 13 or newer (tested with WinLibs GCC 15.2 UCRT, POSIX threads, SEH) or MSVC 2022. MSVC needs a Windows SDK that provides `gameinput.lib` and `xaudio2.lib` (checked with 10.0.26100).
  - **Relinker only (any OS):** GCC, Clang or MSVC.
- Python 3.10 or newer for the tool tests and the end-to-end tests (optional; without it those tests are not registered). `pip install jsonschema` enables the schema check of run reports.

## Full build (Windows)

The libraries need the submodules under `3rdparty/` (Vulkan-Headers). Clone with `git clone --recursive`, or run `git submodule update --init` in an existing clone.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Outputs:

- `build/anybox-relinker.exe`
- `build/anybox-module-probe.exe`, used by `tools/module_check.py`
- `build/anybox-gpu-report.exe`, used by `tools/gpu_report.py`
- `build/libs/ab_*.dll`, including `ab_runtime.dll`
- `build/libs/anybox-libs.map`, the DLL routing table

## Relinker only

Builds and tests the converter without the Windows libraries, on Linux, macOS or Windows:

```
cmake -S . -B build -G Ninja -DANYBOX_RELINKER_ONLY=ON -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## After adding a library

The routing table and the library list are read at configure time. Re-run the `cmake -S . -B build` step after you add a folder under `core/libs/xdll` or edit an `Imports.txt` or `Ordinals.txt`. `CONFIGURE_DEPENDS` makes the build re-run it automatically in most generators.
