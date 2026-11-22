import argparse
import hashlib
import json
import os
import struct
import sys
from pathlib import Path

FORMAT = "anybox-shader-store"
VERSION = 2
INDEX = "modules.txt"
DETAILS = "modules.json"
STAGES = {"vs", "ps", "gs", "hs", "ds", "cs", "ms", "as", "lib"}
SUBGROUP_SIZES = {None: 0, 32: 32, 64: 64}


class StoreError(Exception):
    pass


def container_size(data):
    if len(data) < 32 or data[:4] != b"DXBC":
        raise StoreError("not a DXBC container")
    (size,) = struct.unpack_from("<I", data, 24)
    if size < 32 or size > len(data):
        raise StoreError("container size field is outside the data")
    return size


def container_hash(data):
    return hashlib.sha256(data[:container_size(data)]).hexdigest()


def digest(path):
    hasher = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            hasher.update(block)
    return hasher.hexdigest()


def load_json(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise StoreError(f"{path}: {error}")


def collect(carved, translated):
    carve = load_json(carved / "index.json")
    translation = load_json(translated / "index.json")
    if not isinstance(carve, list) or not isinstance(translation, dict):
        raise StoreError("index.json files are not a carve list and a translation map")
    modules, problems = {}, {"missing": [], "conflicts": [], "invalid": []}
    duplicates = 0
    for entry in carve:
        name = entry.get("file", "")
        stem = name[:-5] if name.endswith(".dxbc") else name
        details = translation.get(stem)
        module = translated / (stem + ".spv")
        if details is None or not module.is_file():
            problems["missing"].append(stem)
            continue
        stage = details.get("stage")
        subgroup = details.get("subgroup_size")
        if stage not in STAGES or isinstance(subgroup, bool) or subgroup not in SUBGROUP_SIZES:
            problems["invalid"].append(stem)
            continue
        try:
            key = container_hash((carved / name).read_bytes())
        except (OSError, StoreError):
            problems["invalid"].append(stem)
            continue
        record = {"name": stem, "stage": stage, "subgroup": SUBGROUP_SIZES[subgroup], "file": module, "sha256": None, "details": details}
        if key in modules:
            duplicates += 1
            first = modules[key]
            if first.get("conflict"):
                continue
            first["sha256"] = first["sha256"] or digest(first["file"])
            if first["stage"] != stage or first["subgroup"] != record["subgroup"] or first["sha256"] != digest(module):
                first["conflict"] = True
                problems["conflicts"].append(key)
            continue
        modules[key] = record
    for key in problems["conflicts"]:
        modules.pop(key, None)
    return modules, duplicates, problems


def write(store, modules, translated):
    store.mkdir(parents=True, exist_ok=True)
    root = os.path.relpath(translated, store)
    lines, details = [], {}
    for key in sorted(modules):
        module = modules[key]
        path = Path(root) / module["file"].name
        if "\t" in str(path) or "\n" in str(path):
            raise StoreError(f"{module['name']}: path holds a tab or newline")
        lines.append(f"{key}\t{module['stage']}\t{module['subgroup']}\t{path.as_posix()}\n")
        details[key] = {"name": module["name"], "stage": module["stage"], "file": path.as_posix(), "translation": module["details"]}
    (store / INDEX).write_text("".join(lines), encoding="utf-8", newline="\n")
    (store / DETAILS).write_text(json.dumps({"format": FORMAT, "version": VERSION, "modules": details}, indent=1) + "\n", encoding="utf-8")


def read_index(store):
    entries = {}
    for number, line in enumerate((store / INDEX).read_text(encoding="utf-8").splitlines(), 1):
        fields = line.split("\t")
        if len(fields) != 4 or len(fields[0]) != 64 or fields[1] not in STAGES or fields[2] not in ("0", "32", "64") or not fields[3]:
            raise StoreError(f"{store / INDEX}:{number}: not <sha256> <stage> <subgroup> <path>")
        entries[fields[0]] = {"stage": fields[1], "subgroup": int(fields[2]), "file": fields[3]}
    return entries


def build(args):
    modules, duplicates, problems = collect(args.carved.resolve(), args.translated.resolve())
    if not modules:
        raise StoreError("no shader could be stored")
    write(args.store.resolve(), modules, args.translated.resolve())
    print(f"{len(modules)} modules stored in {args.store / INDEX}; {duplicates} containers duplicate another, "
          f"{len(problems['missing'])} have no translation, {len(problems['invalid'])} are invalid, "
          f"{len(problems['conflicts'])} hashes dropped because their copies translate differently")
    return 0 if not problems["missing"] and not problems["invalid"] and not problems["conflicts"] else 1


def lookup(args):
    entries = read_index(args.store)
    key = container_hash(args.container.read_bytes())
    entry = entries.get(key)
    if entry is None:
        print(f"{key}: not in the store")
        return 1
    print(f"{key}: {entry['stage']} subgroup {entry['subgroup'] or 'any'} {(args.store / entry['file']).resolve()}")
    return 0


def main():
    parser = argparse.ArgumentParser(description="Build or query the shader store: SPIR-V modules for a title's console shaders, keyed by the SHA-256 of each DXBC container.")
    commands = parser.add_subparsers(dest="command", required=True)
    make = commands.add_parser("build", help="import a translated shader set")
    make.add_argument("carved", type=Path, help="folder of carved .dxbc containers with the carve index.json")
    make.add_argument("translated", type=Path, help="folder of .spv modules with the translation index.json")
    make.add_argument("store", type=Path, help="folder to write modules.txt and modules.json into")
    find = commands.add_parser("lookup", help="find the module for one container")
    find.add_argument("store", type=Path)
    find.add_argument("container", type=Path)
    args = parser.parse_args()
    try:
        return build(args) if args.command == "build" else lookup(args)
    except StoreError as error:
        print(f"shader_store: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
