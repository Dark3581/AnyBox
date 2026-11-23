import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEBT = ROOT / "docs" / "dev" / "debt"
INDEX = ROOT / "docs" / "dev" / "TechnicalDebt.md"
CHANGES = DEBT / "changes"
OPERATION = re.compile(r"^## (add|change|remove) (.+)$")


class FoldError(Exception):
    pass


def debt_path(name):
    return INDEX if name == "TechnicalDebt" else DEBT / f"{name}.md"


def parse_fragment(path):
    operations = []
    target = None
    current = None
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if line.startswith("# "):
            target = line[2:].strip()
            current = None
        elif match := OPERATION.match(line):
            if target is None:
                raise FoldError(f"{path.name}:{number}: operation before a '# <debt file>' line")
            kind, argument = match.groups()
            if kind != "add":
                if not (argument.startswith("[") and argument.endswith("]")):
                    raise FoldError(f"{path.name}:{number}: {kind} needs [text that names one entry]")
                argument = argument[1:-1]
            current = {"file": target, "kind": kind, "argument": argument, "entries": [], "where": f"{path.name}:{number}"}
            operations.append(current)
        elif line.startswith("- "):
            if current is None or current["kind"] == "remove":
                raise FoldError(f"{path.name}:{number}: entry outside an add or change operation")
            current["entries"].append(line)
        elif line.startswith("  ") and current is not None and current["entries"]:
            current["entries"][-1] += "\n" + line
        elif line.strip():
            raise FoldError(f"{path.name}:{number}: unexpected line {line!r}")
    for operation in operations:
        if operation["kind"] != "remove" and not operation["entries"]:
            raise FoldError(f"{operation['where']}: {operation['kind']} has no entries")
        if operation["kind"] == "change" and len(operation["entries"]) != 1:
            raise FoldError(f"{operation['where']}: change replaces exactly one entry")
    return operations


def split_entries(lines):
    entries = []
    for index, line in enumerate(lines):
        if line.startswith("- "):
            end = index + 1
            while end < len(lines) and lines[end].startswith("  "):
                end += 1
            entries.append((index, end))
    return entries


def section_bounds(lines, heading):
    start = next((i for i, line in enumerate(lines) if line.strip() == f"### {heading}"), None)
    if start is None:
        return None
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("#")), len(lines))
    return start, end


def apply(text, operation, title):
    lines = text.splitlines()
    if operation["kind"] == "add":
        bounds = section_bounds(lines, operation["argument"])
        new = [line for entry in operation["entries"] for line in entry.split("\n")]
        if bounds is None:
            while lines and not lines[-1].strip():
                lines.pop()
            lines += ["", f"### {operation['argument']}", ""] + new
            return "\n".join(lines) + "\n"
        start, end = bounds
        last = start + 1
        for entry_start, entry_end in split_entries(lines[start:end]):
            last = start + entry_end
        insert_at = last if last > start + 1 else start + 1
        if insert_at == start + 1 and (insert_at >= len(lines) or lines[insert_at].strip()):
            new = [""] + new
        lines[insert_at:insert_at] = new
        return "\n".join(lines) + "\n"
    matches = [(s, e) for s, e in split_entries(lines) if operation["argument"] in "\n".join(lines[s:e])]
    if len(matches) != 1:
        raise FoldError(f"{operation['where']}: [{operation['argument']}] matches {len(matches)} entries of {title}, not exactly one")
    start, end = matches[0]
    lines[start:end] = [] if operation["kind"] == "remove" else operation["entries"][0].split("\n")
    return "\n".join(lines) + "\n"


def fold(paths, only=None):
    results = {}
    touched = {}
    for path in paths:
        for operation in parse_fragment(path):
            name = operation["file"]
            if only is not None and name != only:
                continue
            if operation["kind"] != "add":
                key = (name, operation["argument"])
                if key in touched:
                    raise FoldError(f"{operation['where']} and {touched[key]} change the same entry")
                touched[key] = operation["where"]
            target = debt_path(name)
            if name not in results:
                if target.exists():
                    results[name] = target.read_text(encoding="utf-8")
                elif operation["kind"] == "add":
                    results[name] = f"# {name} technical debt\n"
                else:
                    raise FoldError(f"{operation['where']}: {target.relative_to(ROOT)} does not exist")
            results[name] = apply(results[name], operation, name)
    return results


def fragments():
    return sorted(path for path in CHANGES.glob("*.md") if path.name != "README.md")


def main():
    parser = argparse.ArgumentParser(description="Apply docs/dev/debt/changes fragments to the debt files.")
    parser.add_argument("--check", action="store_true", help="validate the fragments without writing")
    parser.add_argument("--show", metavar="NAME", help="print one debt file with the pending fragments applied")
    args = parser.parse_args()
    try:
        results = fold(fragments(), args.show)
    except FoldError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    if args.show:
        path = debt_path(args.show)
        sys.stdout.write(results.get(args.show, path.read_text(encoding="utf-8") if path.exists() else ""))
        return 0
    if args.check:
        print(f"{len(fragments())} fragments apply cleanly")
        return 0
    for name, text in results.items():
        debt_path(name).write_text(text, encoding="utf-8")
    for path in fragments():
        path.unlink()
    import readme_docs
    readme = readme_docs.README.read_text(encoding="utf-8")
    fresh = readme_docs.updated(readme)
    if fresh != readme:
        readme_docs.README.write_text(fresh, encoding="utf-8", newline="\n")
    print(f"folded into {len(results)} debt files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
