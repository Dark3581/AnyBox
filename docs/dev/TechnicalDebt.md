# Project technical debt

<p align="center">
  <a href="../../README.md">Home</a> &nbsp;·&nbsp;
  <a href="../user/USAGE.md">Usage</a> &nbsp;·&nbsp;
  <a href="BUILD.md">Build</a> &nbsp;·&nbsp;
  <a href="ARCHITECTURE.md">Architecture</a> &nbsp;·&nbsp;
  <a href="GAME_TESTING.md">Game testing</a> &nbsp;·&nbsp;
  <a href="../../CONTRIBUTING.md">Contributing</a> &nbsp;·&nbsp;
  <a href="CONVENTIONS.md">Conventions</a> &nbsp;·&nbsp;
  <b>Technical debt</b> &nbsp;·&nbsp;
  <a href="PROGRESS.md">Progress</a>
</p>

Known gaps: what differs from the console, what is unverified, and what stops a title. Entries about one library or component are in [debt](debt/), one file per library, under the same `###` sections as this page. An entry may end with `Closes when:` and the measurement, title or test that would close it, so contributors can search for the ones they can re-check.

#### Fragments

A pull request does not edit the debt files. It adds its own fragment, `docs/dev/debt/changes/<branch>.md`, with a `# <name>` line per debt file (`TechnicalDebt` for this page) and operations under it:

```markdown
# xgameruntime

## add Silent stubs

- [XFoo](../../../core/libs/xdll/xgameruntime/Inventory.cpp) (xgameruntime) - returns S_OK without reading its argument

## change [XTaskQueueTerminate]

- [XTaskQueueTerminate](../../../core/libs/xdll/xgameruntime/TaskQueue.cpp) (xgameruntime) - cancellation order measured on a console

## remove [XBaz]
```

`add` puts entries after the last entry of that `###` section, creating the section if needed. `change` and `remove` name text that occurs in exactly one entry of the file. `python tools/debt_fold.py --check` validates fragments (CI runs it), `--show <name>` prints a debt file with pending fragments applied, and running it without options applies and deletes every fragment.

### Silent stubs

Every function either does exactly what it is supposed to or stops the title through `AbxNotImplemented` or `AbxUnsupported`. The exceptions are listed under **Silent stubs** in each library's debt file.

### Platform

- Only Windows hosts run converted titles. A Linux host would need a PE loader for the converted image and the AnyBox DLLs.
- Titles must already be decrypted, plain PE files. AnyBox does not read or unpack protected packages.
- A title's own unhandled exception filter, installed after start-up, replaces AnyBox's, and its crashes then produce no run report.
- `run.exitCode` is always `null`: the report is written before the exit code is known.
