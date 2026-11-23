import argparse
import json
import os
import struct
import sys
from pathlib import Path

import shader_scan

STAGES = {0: "ps", 1: "vs", 2: "gs", 3: "hs", 4: "ds", 5: "cs", 6: "lib", 13: "ms", 14: "as"}
MODULE_SUFFIXES = (".exe", ".dll")
NATIVE_MARKER = b"XBCH"
INDEX = "index.json"


def native_containers(data):
    offset = data.find(shader_scan.MAGIC)
    while offset != -1:
        found = shader_scan.container_at(data, offset)
        if found is None:
            offset = data.find(shader_scan.MAGIC, offset + 4)
            continue
        size, parts = found
        program = next(((start, length) for tag, start, length in parts if tag == "DXIL"), None)
        if program is not None and program[1] >= 4 and NATIVE_MARKER in data[program[0]:program[0] + program[1]]:
            version = struct.unpack_from("<I", data, program[0])[0]
            stage = STAGES.get(version >> 16)
            if stage is not None:
                yield {"offset": offset, "size": size, "stage": stage, "sm": f"{(version >> 4) & 0xF}.{version & 0xF}",
                       "parts": {tag: length for tag, _, length in parts}}
        offset = data.find(shader_scan.MAGIC, offset + size)


def package_modules(package):
    return sorted(path for path in package.iterdir() if path.is_file() and path.suffix.lower() in MODULE_SUFFIXES)


def carve(package, out):
    out.mkdir(parents=True, exist_ok=True)
    index = []
    for module in package_modules(package):
        data = module.read_bytes()
        for container in native_containers(data):
            name = f"{module.stem}_{container['offset']:08x}"
            file = name + ".dxbc"
            (out / file).write_bytes(data[container["offset"]:container["offset"] + container["size"]])
            index.append({"name": name, "file": file, "source": module.name, **container})
    temporary = out / (INDEX + ".tmp")
    temporary.write_text(json.dumps(index, indent=1) + "\n", encoding="utf-8")
    os.replace(temporary, out / INDEX)
    return index


def main(argv=None):
    parser = argparse.ArgumentParser(description="Carve the native-code shader containers that a title's executable and libraries embed, in the carve index layout the shader translator reads.")
    parser.add_argument("package", type=Path, help="the title's package folder; its top-level .exe and .dll files are scanned")
    parser.add_argument("out", type=Path, help="folder for the .dxbc files and index.json")
    args = parser.parse_args(argv)
    if not args.package.is_dir():
        print(f"embedded_shaders: {args.package} is not a folder", file=sys.stderr)
        return 1
    index = carve(args.package, args.out)
    sources = {}
    for entry in index:
        sources[entry["source"]] = sources.get(entry["source"], 0) + 1
    print(f"embedded_shaders: {len(index)} containers from {len(sources)} modules" + "".join(f"; {name} {count}" for name, count in sorted(sources.items())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
