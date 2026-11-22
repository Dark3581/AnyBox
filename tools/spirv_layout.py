import argparse
import json
import struct
import sys
from pathlib import Path

MAGIC = 0x07230203
OP_DECORATE, OP_VARIABLE, OP_CONSTANT, OP_FUNCTION = 71, 59, 43, 54
OP_TYPE_INT, OP_TYPE_FLOAT = 21, 22
OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER = 25, 26, 28, 29, 30, 32
DECORATION_BINDING, DECORATION_SET = 33, 34
STORAGE_UNIFORM_CONSTANT, STORAGE_UNIFORM, STORAGE_STORAGE_BUFFER = 0, 2, 12
DIMS = {0: "1d", 1: "2d", 2: "3d"}
FORMATS = {0: "unknown", 33: "r32ui"}
FIELDS = ("type", "dim", "arrayed", "component", "format")


class LayoutError(Exception):
    pass


def check_header(data):
    if len(data) < 20 or len(data) % 4:
        raise LayoutError("not a whole number of SPIR-V words")
    if struct.unpack_from("<I", data, 0)[0] != MAGIC:
        raise LayoutError("no SPIR-V magic")
    return len(data) // 4


def descriptor_bindings(data):
    count = check_header(data)
    types, constants, binding, descriptor_set, variables = {}, {}, {}, {}, []
    wanted = (OP_DECORATE, OP_TYPE_INT, OP_TYPE_FLOAT, OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER, OP_CONSTANT, OP_VARIABLE)
    index = 5
    while index < count:
        (word,) = struct.unpack_from("<I", data, index * 4)
        opcode, length = word & 0xFFFF, word >> 16
        if length == 0 or index + length > count:
            raise LayoutError(f"bad instruction length at word {index}")
        if opcode == OP_FUNCTION:
            break
        if opcode not in wanted:
            index += length
            continue
        operands = struct.unpack_from(f"<{length - 1}I", data, index * 4 + 4)
        if opcode == OP_DECORATE and len(operands) >= 3 and operands[1] in (DECORATION_BINDING, DECORATION_SET):
            (binding if operands[1] == DECORATION_BINDING else descriptor_set)[operands[0]] = operands[2]
        elif opcode in (OP_TYPE_INT, OP_TYPE_FLOAT, OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER):
            types[operands[0]] = (opcode, operands[1:])
        elif opcode == OP_CONSTANT and len(operands) >= 3:
            constants[operands[1]] = operands[2]
        elif opcode == OP_VARIABLE:
            variables.append((operands[1], operands[0], operands[2]))
        index += length
    found = {}
    for variable, pointer, storage in variables:
        if variable not in binding:
            continue
        if storage not in (STORAGE_UNIFORM_CONSTANT, STORAGE_UNIFORM, STORAGE_STORAGE_BUFFER):
            raise LayoutError(f"binding {binding[variable]} is in storage class {storage}")
        opcode, rest = types.get(pointer, (None, ()))
        if opcode != OP_TYPE_POINTER:
            raise LayoutError(f"binding {binding[variable]} has no pointer type")
        opcode, rest = types.get(rest[1], (None, ()))
        count = 1
        if opcode == OP_TYPE_RUNTIME_ARRAY:
            count = None
            opcode, rest = types.get(rest[0], (None, ()))
        elif opcode == OP_TYPE_ARRAY:
            count = constants.get(rest[1])
            opcode, rest = types.get(rest[0], (None, ()))
        if opcode == OP_TYPE_IMAGE:
            dim, arrayed, sampled = rest[1], rest[3], rest[5]
            if dim not in DIMS or sampled not in (1, 2):
                raise LayoutError(f"binding {binding[variable]} is an image of dim {dim}, sampled {sampled}")
            component_type = types.get(rest[0], (None, ()))
            if component_type[0] == OP_TYPE_FLOAT:
                component = "float"
            elif component_type[0] == OP_TYPE_INT:
                component = "sint" if component_type[1][1] else "uint"
            else:
                raise LayoutError(f"binding {binding[variable]} samples an unsupported component type")
            entry = {"type": "sampled_image" if sampled == 1 else "storage_image", "dim": DIMS[dim], "arrayed": bool(arrayed),
                     "component": component, "format": FORMATS.get(rest[6], f"format{rest[6]}")}
        elif opcode == OP_TYPE_SAMPLER:
            entry = {"type": "sampler", "dim": "none", "arrayed": False, "component": "none", "format": "none"}
        elif opcode == OP_TYPE_STRUCT and storage in (STORAGE_STORAGE_BUFFER, STORAGE_UNIFORM):
            entry = {"type": "storage_buffer" if storage == STORAGE_STORAGE_BUFFER else "uniform_buffer", "dim": "none", "arrayed": False, "component": "none", "format": "none"}
        else:
            raise LayoutError(f"binding {binding[variable]} has an unsupported type (opcode {opcode})")
        entry["count"] = count
        found[(descriptor_set.get(variable, 0), binding[variable])] = entry
    return found


def mismatches(found, layout):
    expected = {(layout["set"], b["binding"]): b for b in layout["bindings"]}
    problems = []
    for key, entry in sorted(found.items()):
        spec = expected.get(key)
        if spec is None:
            problems.append(f"set {key[0]} binding {key[1]} is not in the layout")
            continue
        for field in FIELDS:
            if entry[field] != spec[field]:
                problems.append(f"binding {key[1]}: {field} {entry[field]}, the layout has {spec[field]}")
        if entry["count"] is not None and entry["count"] > spec["count"]:
            problems.append(f"binding {key[1]}: {entry['count']} descriptors, the layout has {spec['count']}")
    return problems


def main():
    parser = argparse.ArgumentParser(description="Compare the descriptor bindings every SPIR-V module declares with a title layout (anybox-pipeline-probe layout).")
    parser.add_argument("layout", type=Path, help="layout JSON from anybox-pipeline-probe layout")
    parser.add_argument("modules", type=Path, nargs="+", help=".spv files")
    args = parser.parse_args()
    layout = json.loads(args.layout.read_text(encoding="utf-8"))
    failed = 0
    for module in args.modules:
        try:
            problems = mismatches(descriptor_bindings(module.read_bytes()), layout)
        except (OSError, LayoutError) as error:
            problems = [str(error)]
        for problem in problems:
            print(f"{module}: {problem}")
        failed += bool(problems)
    print(f"{len(args.modules) - failed} of {len(args.modules)} modules fit the layout")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
