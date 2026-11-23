import argparse
import json
import sys
from pathlib import Path

REQUIRED = ("format", "version", "generated", "anybox", "game", "system", "run", "libraries", "truncated", "stop")
KINDS = ("none", "not-implemented", "unsupported", "crash")


def validate(report):
    problems = [f"missing {key}" for key in REQUIRED if key not in report]
    if report.get("format") != "anybox-report":
        problems.append("format is not anybox-report")
    if report.get("version") != 1:
        problems.append("version is not 1")
    if report.get("stop", {}).get("kind") not in KINDS:
        problems.append(f"stop.kind is not one of {', '.join(KINDS)}")
    try:
        import jsonschema
    except ImportError:
        return problems
    schema = json.loads((Path(__file__).resolve().parent.parent / "docs" / "run-report.schema.json").read_text(encoding="utf-8"))
    validator = jsonschema.Draft202012Validator(schema)
    problems += [f"{'/'.join(map(str, error.path)) or '(root)'}: {error.message}" for error in validator.iter_errors(report)]
    return problems


def summary(report):
    game = report["game"]
    stop = report["stop"]
    title = f"{game.get('name') or 'Unknown title'} ({game.get('titleId') or 'no title ID'})"
    if stop["kind"] == "none":
        ending = "ran to exit"
    elif stop["kind"] == "crash":
        exception = stop.get("exception") or {}
        ending = f"crashed {exception.get('code')} in {exception.get('module')}+{exception.get('offset')}"
    else:
        ending = f"stopped at {stop['kind']} {stop.get('library')}!{stop.get('function')}"
        if stop.get("message"):
            ending += f": {stop['message']}"
        caller = stop.get("caller")
        if caller and caller.get("module"):
            ending += f" (called from {caller['module']}+{caller.get('offset')})"
    commit = (report["anybox"].get("commit") or "unknown")[:10]
    system = report["system"]
    return f"{title} v{game.get('version') or '?'}: {ending} after {report['run']['seconds']:.0f} s | AnyBox {commit} | {system.get('osVersion')} | {system.get('cpu')}"


def main():
    parser = argparse.ArgumentParser(description="Check an anybox-report.json and print the line to paste into a Game bring-up thread.")
    parser.add_argument("reports", nargs="+", type=Path)
    args = parser.parse_args()
    status = 0
    for path in args.reports:
        report = json.loads(path.read_text(encoding="utf-8"))
        problems = validate(report)
        for problem in problems:
            print(f"{path}: {problem}", file=sys.stderr)
        if problems:
            status = 1
            continue
        print(summary(report))
    return status


if __name__ == "__main__":
    sys.exit(main())
