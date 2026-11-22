import argparse
import json
import mmap
import struct
import re
import sys
import zlib
from collections import Counter
from pathlib import Path

MAGIC = b"DXBC"
HEADER = 32
MAX_PARTS = 64
CODE_PARTS = {"DXIL": "dxil", "SHEX": "dxbc", "SHDR": "dxbc"}
BITCODE = b"BC\xc0\xde"
MAX_STORED = 64 << 20
MAX_INFLATED = 256 << 20
ENTRY = re.compile(rb"\x00.(?P<stored>.{4}).{4}(?P=stored)(?P<size>.{4})", re.DOTALL)
NAME = re.compile(rb"[\x21-\x7e]{3,200}\Z")


def container_at(data, offset):
    if offset + HEADER > len(data):
        return None
    major, minor, size, count = struct.unpack_from("<HHII", data, offset + 20)
    if (major, minor) != (1, 0) or size < HEADER or offset + size > len(data) or not 0 < count <= MAX_PARTS or HEADER + count * 4 > size:
        return None
    parts = []
    for index in range(count):
        part = struct.unpack_from("<I", data, offset + HEADER + index * 4)[0]
        if part < HEADER + count * 4 or part + 8 > size:
            return None
        fourcc = bytes(data[offset + part:offset + part + 4])
        length = struct.unpack_from("<I", data, offset + part + 4)[0]
        if part + 8 + length > size or not all(0x20 <= c < 0x7F for c in fourcc):
            return None
        parts.append((fourcc.decode("ascii"), offset + part + 8, length))
    return size, parts


def dxil_program(data, start, length):
    if length < 24 or bytes(data[start + 8:start + 12]) != b"DXIL":
        return "unknown"
    bitcode_offset, bitcode_size = struct.unpack_from("<II", data, start + 16)
    begin = start + 8 + bitcode_offset
    if bitcode_size < 4 or 8 + bitcode_offset + bitcode_size > length:
        return "unknown"
    magic = bytes(data[begin:begin + 4])
    if magic == BITCODE:
        return "bitcode"
    if magic == b"NOBC":
        return "native" if b"XBCH" in data[start:start + length] else "stub"
    return "unknown"


def describe(data, offset, found):
    size, parts = found
    entry = {"offset": offset, "size": size, "kind": kind([tag for tag, _, _ in parts]), "parts": [tag for tag, _, _ in parts]}
    program = next(((start, length) for tag, start, length in parts if tag == "DXIL"), None)
    if program is not None:
        entry["program"] = dxil_program(data, *program)
    return entry


def kind(parts):
    kinds = {CODE_PARTS[part] for part in parts if part in CODE_PARTS}
    if len(kinds) == 1:
        return kinds.pop()
    if kinds:
        return "mixed"
    return "rootsig" if "RTS0" in parts else "other"


def scan_file(path):
    containers = []
    if path.stat().st_size < HEADER:
        return containers
    with path.open("rb") as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
        offset = data.find(MAGIC)
        while offset != -1:
            found = container_at(data, offset)
            if found:
                containers.append(describe(data, offset, found))
                offset = data.find(MAGIC, offset + found[0])
            else:
                offset = data.find(MAGIC, offset + 1)
        if bytes(data[:4]) == b"9FPR":
            containers += deflate_entries(data)
    return containers


def deflate_entries(data):
    containers = []
    for match in ENTRY.finditer(data):
        stored = struct.unpack("<I", match.group("stored"))[0]
        size = struct.unpack("<I", match.group("size"))[0]
        if not HEADER <= size <= MAX_INFLATED or not 0 < stored <= MAX_STORED:
            continue
        start = match.end()
        if start + stored > len(data):
            continue
        try:
            inflated = zlib.decompressobj(-zlib.MAX_WBITS).decompress(data[start:start + stored], size + 1)
        except zlib.error:
            continue
        if len(inflated) != size or inflated[:4] != MAGIC:
            continue
        found = container_at(inflated, 0)
        if not found or found[0] != size:
            continue
        entry = describe(inflated, match.start(), found)
        entry["stored"] = "deflate"
        name = NAME.search(data, max(0, match.start() - 200), match.start())
        if name:
            entry["name"] = name.group().decode("ascii")
        containers.append(entry)
    return containers


def collect(paths):
    files = []
    for path in paths:
        files += sorted(p for p in path.rglob("*") if p.is_file()) if path.is_dir() else [path]
    return files


def summarize(results):
    kinds, parts, programs, stored = Counter(), Counter(), Counter(), Counter()
    for containers in results.values():
        for container in containers:
            kinds[container["kind"]] += 1
            parts.update(container["parts"])
            if "program" in container:
                programs[container["program"]] += 1
            stored[container.get("stored", "plain")] += 1
    return {"files": sum(1 for c in results.values() if c), "containers": sum(kinds.values()), "kinds": dict(sorted(kinds.items())),
            "parts": dict(sorted(parts.items())), "programs": dict(sorted(programs.items())), "stored": dict(sorted(stored.items()))}


def main():
    parser = argparse.ArgumentParser(description="Count the DXBC shader containers in a GDK title and the parts each one holds (DXIL, DXBC bytecode, root signature or unknown).")
    parser.add_argument("paths", nargs="+", type=Path, help="files or package folders (decrypted)")
    parser.add_argument("--json", type=Path, help="write the summary and per-file containers as JSON: names, offsets and part tags only")
    args = parser.parse_args()

    results = {}
    for path in collect(args.paths):
        try:
            containers = scan_file(path)
        except OSError as error:
            print(f"{path}: {error}", file=sys.stderr)
            continue
        if containers:
            results[str(path)] = containers
            print(f"{path}: {', '.join(f'{n} {k}' for k, n in sorted(Counter(c['kind'] for c in containers).items()))}")

    summary = summarize(results)
    print(f"{summary['containers']} containers in {summary['files']} files; kinds: {summary['kinds']}; programs: {summary['programs']}; "
          f"stored: {summary['stored']}; parts: {summary['parts']}")
    if args.json:
        args.json.write_text(json.dumps({"summary": summary, "files": results}, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
