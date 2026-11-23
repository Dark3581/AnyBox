# Progress tracking

<p align="center">
  <a href="../../README.md">Home</a> &nbsp;·&nbsp;
  <a href="../user/USAGE.md">Usage</a> &nbsp;·&nbsp;
  <a href="BUILD.md">Build</a> &nbsp;·&nbsp;
  <a href="ARCHITECTURE.md">Architecture</a> &nbsp;·&nbsp;
  <a href="GAME_TESTING.md">Game testing</a> &nbsp;·&nbsp;
  <a href="../../CONTRIBUTING.md">Contributing</a> &nbsp;·&nbsp;
  <a href="CONVENTIONS.md">Conventions</a> &nbsp;·&nbsp;
  <a href="TechnicalDebt.md">Technical debt</a> &nbsp;·&nbsp;
  <b>Progress</b>
</p>

What is counted, where it is published, and how to read it.

## Library counts

[`tools/progress.py`](../../tools/progress.py) scans `core/libs/xdll/<library>/` (excluding `tests/`) through [`tools/anybox_scan.py`](../../tools/anybox_scan.py):

- **implemented:** an `ABX_API` function definition whose body does not call `AbxNotImplemented`;
- **stub:** an `ABX_API` definition whose body calls `AbxNotImplemented`;
- **forwarded:** a `Name=dll.Target` line in the library's `Forwards.def`.

Comments, string and character literals are blanked before matching, so braces or names inside them do not count. The percentage is `(implemented + forwarded) / declared`. A count says nothing about whether a title runs; compatibility lives in the Game bring-up discussions.

Progress also counts what AnyBox does not declare yet, from the [export inventories](../inventory) and [title needs records](../needs):

- **not declared:** a function a library's DLLs are known to export or titles import, with no declaration. `known = declared + not declared`, and `knownPercent` is done of known.
- **export list not known yet** (`*` on the map): no inventory or needs record covers the library's DLLs, so its known total is just what it declares.
- **DLLs with no library** (`unserved`): DLLs that titles import or inventories list, that no library serves and Windows does not provide. Each is a dashed red tile on the map and a row on the page with the `stubgen.py --new-library` command that starts it.

```
python tools/progress.py out/            # progress.json, progress.html, progress.svg, badge-libraries.svg
python tools/progress.py --module xgameruntime
python tools/progress.py --json
python tools/progress.py --compare base.json head.json   # the pull request comment; empty when no library changed
python tools/progress.py --root ../other-checkout --json  # count another checkout with this version of the tool
```

## History

[`tools/progress_history.py`](../../tools/progress_history.py) measures the first-parent history: one point per UTC day, the last commit of that day, read through `git show` with today's counting rules. It writes `history.json`, `history.svg` and `history.html`. `--cache <file>` keeps measured commits between runs.

## Publishing

- [Pages workflow](../../.github/workflows/pages.yml): on pushes to `main` that touch `core/libs`, the tools or the docs, it publishes the progress page, map, badge and history to GitHub Pages.
- [Progress report workflow](../../.github/workflows/progress-report.yml): on pull requests, it counts the base (`--root`) and the merged head with the same version of the tool and compares them. [Progress comment](../../.github/workflows/progress-comment.yml) posts the result on the pull request as one comment, updated on each push and deleted when the change no longer touches a library: the new percentage and its change, the functions implemented, forwarded, newly stubbed, removed or no longer done, and a table of the changed libraries. It runs in a separate workflow because pull requests from forks get a read-only token.

## What still needs implementing

Progress counts only what AnyBox already declares. Three tools find everything else and rank it, without implementing anything:

1. [`tools/lib_inventory.py`](../../tools/lib_inventory.py) lists every export of import libraries (`.lib`, short format) or DLLs, and stores the names in [`docs/inventory`](../inventory) with `--json`. The inventory from the public PC GDK is committed; [its README](../inventory/README.md) says how it was made.
2. [`tools/import_audit.py`](../../tools/import_audit.py) `--needs docs/needs/<TitleId>.json` stores what one title imports, minus host and bundled DLLs, in [`docs/needs`](../needs).
3. [`tools/needs.py`](../../tools/needs.py) merges both with the current library scan:
   - every known function, with its DLL, the library that serves it, its status and the titles that import it, sorted so the functions most titles need and AnyBox lacks come first;
   - per DLL: known exports, how many titles need it, declared, done;
   - per title: how many of its imports are done, and its next blockers.

```
python tools/needs.py site/            # needs.json, needs.html; prints a Markdown summary
python tools/stubgen.py --from-audit site/needs.json   # declares every absent function as a stub
```

The Pages workflow publishes `needs.html` next to the progress page.

## Title imports

[`tools/import_audit.py`](../../tools/import_audit.py) reads a title's PE files and classifies every import:

| Status | Meaning |
|---|---|
| implemented | the routed library implements it |
| forwarded | the routed library forwards it to the host |
| stub | declared; calling it stops the title with a run report |
| absent | the DLL is routed but the function is not declared; `stubgen.py --from-audit` adds it |
| bundled | the title ships the DLL itself |
| host | desktop Windows provides the DLL |
| unrouted | nothing serves the DLL; start a library with `stubgen.py --new-library` |

It exits with 1 when something is absent or unrouted (`--strict-stubs` also fails on stubs).
