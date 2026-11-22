# Conventions

`python tools/check_conventions.py` checks the rules marked ✔. CI runs it on every pull request.

## Code

- ✔ No comments in C and C++ under `core/`, except `#endif` lines, namespace-end lines and an upstream licence notice at the top of an adapted file. Explanations of gaps belong in [technical debt](TechnicalDebt.md).
- ✔ Text files are UTF-8.
- C++20 standard library; the relinker uses nothing else. Libraries may use the Windows API.
- Types and functions `PascalCase`, locals and parameters `camelCase`, members `camelCase_`, globals `g_camelCase`, thread-locals `t_camelCase`, constants `PascalCase`.
- Exported functions keep their console names exactly (`XTaskQueueCreate`).
- Library-internal code lives in an anonymous namespace.

## Libraries

- ✔ A library folder name is lowercase letters, digits and underscores, and has `Imports.txt`, `<name>.hpp` and `CMakeLists.txt`.
- ✔ A DLL name pattern is served by one library only.
- ✔ Every `ABX_API` definition has exactly one declaration, in a header; none in source files. Every declaration has a definition.
- ✔ A function is either defined or forwarded, not both.
- Stubs call `AbxNotImplemented(__func__)`. Unknown behaviour inside an implemented function calls `AbxUnsupported(__func__, "<what is unknown>")`.
- Implemented functions trace each call with `AbxTrace("<Function>(<arguments>) -> <result>", ...)`: arguments in call order, pointers with `%p`, flags and codes in hex.
- Signatures not taken from a public header are declared without parameters and listed under **Unknown function info** in the library's debt file.
- Implementation files are named after the API area (`TaskQueue.cpp`, `GameSave.cpp`); unimplemented stubs live in `Inventory.cpp`.

## Commits and pull requests

- ✔ Commit subjects are [Conventional Commits](https://www.conventionalcommits.org): `type(scope): summary`, with type one of `feat fix docs style refactor perf test build ci chore revert`. The scope is a library (`xgameruntime`), `relinker`, `runtime`, `tools`, `ci` or `docs`.
- PR titles follow the same format.
