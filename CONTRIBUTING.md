# Contributing

<p align="center">
  <a href="README.md">Home</a> &nbsp;·&nbsp;
  <a href="docs/user/USAGE.md">Usage</a> &nbsp;·&nbsp;
  <a href="docs/dev/BUILD.md">Build</a> &nbsp;·&nbsp;
  <a href="docs/dev/ARCHITECTURE.md">Architecture</a> &nbsp;·&nbsp;
  <a href="docs/dev/GAME_TESTING.md">Game testing</a> &nbsp;·&nbsp;
  <b>Contributing</b> &nbsp;·&nbsp;
  <a href="docs/dev/CONVENTIONS.md">Conventions</a> &nbsp;·&nbsp;
  <a href="docs/dev/TechnicalDebt.md">Technical debt</a> &nbsp;·&nbsp;
  <a href="docs/dev/PROGRESS.md">Progress</a>
</p>

Thanks for helping. AnyBox grows one function at a time, and most contributions are small: implement one stub, forward one export, or add the stubs a new title needs. This page is everything you need to do that.

## Find something to do

- **Needs page.** The [needs page](https://dark3581.github.io/AnyBox/needs.html) ranks every known function by how many titles need it and AnyBox lacks it. Start from the top. Add your own titles to it with `python tools/import_audit.py <package> --needs docs/needs/<TitleId>.json` ([details](docs/needs/README.md)).
- **Progress page.** Every stub is listed per library on the [progress page](https://dark3581.github.io/AnyBox/progress.html). Locally: `python tools/progress.py --module xgameruntime`.
- **A title you own.** `python tools/import_audit.py <decrypted package folder>` lists every import as `implemented`, `forwarded`, `stub`, `absent` (no AnyBox declaration yet) or `unrouted` (no library serves the DLL). Run it before you start, so you work on what the title actually calls.
- **Issues** labeled `good first issue` and `library request`.
- **Known gaps** in [docs/dev/debt](docs/dev/debt), especially entries ending in `Closes when:`.

Search open pull requests for the function name before you start, so two people don't implement the same thing.

## Kinds of change

| You want to | Do this |
|---|---|
| Implement a stub | Replace the `AbxNotImplemented(__func__)` body with the real behaviour, give the declaration its real signature, and add a test. Keep an exact signature from the public GDK headers; do not copy the GDK headers into the repository. |
| Use the host's implementation unchanged | Add `Name=hostdll.Name` to the library's `Forwards.def` and remove its stub. Only when the console and desktop contracts match; record any doubt in debt. |
| Serve an import by ordinal | Add `<Name> @<ordinal>` to the library's `Ordinals.txt`. CMake pins the export at that ordinal, and the relinker refuses a title that imports an ordinal no library pins. |
| Add imports a title needs | `python tools/import_audit.py <package> --json audit.json` then `python tools/stubgen.py --from-audit audit.json`. Commit the generated stubs, declarations and debt fragment. |
| Serve a new DLL | `python tools/stubgen.py --new-library <name> --dll <name>.dll`, then reconfigure CMake. |
| Change the relinker | Build with `-DANYBOX_RELINKER_ONLY=ON` (works on Linux and macOS too) and add a case to [RelinkerTests.cpp](core/relinker/tests/RelinkerTests.cpp). |
| Change a tool | Add or extend a test in [tools/tests](tools/tests); CTest runs them. |

## Rules

- Follow the [conventions](docs/dev/CONVENTIONS.md): naming, no comments in code, Conventional Commits.
- **Implement or stop.** Every function either does exactly what it is supposed to or stops the process through `AbxNotImplemented` / `AbxUnsupported`, which write the run report. A silent stub (returns success without doing the work) is allowed only when it unblocks a title and affects only presentation, and it is listed under **Silent stubs** in its library's debt file.
- Implement the general behaviour of a function, not what one title happens to need. Never branch on a title ID, path or value from one trace.
- Missing imports fail at conversion or start-up with a clear error; never add fallbacks that keep running.
- Every `ABX_API` export has exactly one declaration in a header under its library's `include/`. `tools/check_conventions.py` checks this.
- Don't replace modules that titles ship themselves (engine and middleware DLLs, XSAPI thunks). Only system libraries are reimplemented.
- Behaviour taken from a public header or documentation says so in the PR. Behaviour not checked against a console is recorded in debt.
- Third-party code is a git submodule under `3rdparty/`, built from source with CMake.
- Tests finish in 30 s each.

## Build and test

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
python tools/check_conventions.py
```

Toolchains and the relinker-only build are in [BUILD.md](docs/dev/BUILD.md).

## Pull requests

- One topic per pull request, based on current `main`. Follow-ups go in a new pull request.
- Fill in the [template](.github/pull_request_template.md): what changed, how it was tested, dependencies, the issue it fixes, the title it unblocks (`Unblocks: 4D5307E6 #12`), and whether AI assisted.
- Known gaps are added, changed or removed through a fragment in `docs/dev/debt/changes/<branch>.md`, never by editing the debt files ([format](docs/dev/TechnicalDebt.md#fragments)). Maintainers fold fragments with `python tools/debt_fold.py`.
- Investigation notes, logs and screenshots go in the pull request, not in the repository.
- CI runs the conventions check, the relinker tests on Linux, the full Windows build and tests, and posts the library progress change as a comment.
- Labels are added for you: the area a pull request touches (`libs`, `relinker`, `tools`, `ci`, or `docs` when it changes only documentation; [labeler.yml](.github/labeler.yml)), `fixes issue` when it closes an issue, and `unblocks title` when its `Unblocks:` line names a title ID.
- A pull request that conflicts with `main` gets the `needs rebase` label within the hour. Rebase it on current `main` and force-push; the label goes away by itself. After 7 days in conflict with no activity, it is marked `stale` with a reminder. Nothing is closed automatically.
- Say whether the change was written with AI assistance. The author is responsible for every line.

## Reporting a title

Use the Game bring-up discussion form, one thread per title ID. Include the `anybox-report.json` summary line (`python tools/run_report_summary.py anybox-report.json`), the AnyBox commit and your system. A failed run with a complete report is as useful as a successful one. See [GAME_TESTING.md](docs/dev/GAME_TESTING.md).

Only share material you have the right to distribute. Never attach game files, keys or decrypted executables.
