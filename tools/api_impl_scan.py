import argparse
import json
import re
import shutil
import struct
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

LINE = re.compile(r'^\s+([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)$')
RIP_TARGET = re.compile(r'# 0x([0-9a-f]+)')
DIRECT = re.compile(r'\b(call|jmp)\s+0x([0-9a-f]+)$')
GUID_LEA = re.compile(r'lea\s+(rcx|rdx),\[rip\+0x[0-9a-f]+\]\s+# 0x([0-9a-f]+)')
COPY = re.compile(r'mov\s+(rcx|rdx),(rcx|rdx)$')
SLOT = re.compile(r'mov\s+rax,QWORD PTR \[(?:rax|rdx|rcx|r8|r9|r10|r11)\+0x([0-9a-f]+)\]')
ARGUMENT = re.compile(r'\b(?:mov|movzx|movsx|movsxd|lea|xor|movss|movsd|movaps)\s+(rdx|edx|dx|dl|r8|r8d|r8w|r8b|r9|r9d|r9w|r9b|xmm[123])\b,')
STACK = re.compile(r'(QWORD|DWORD|WORD|BYTE) PTR \[rsp\+0x(2[0-9a-f]|[3-9a-f][0-9a-f])\],')
BOUNDARY = re.compile(r'\bint3\b|\bret\b')


def parse_listing(text):
    rows = []
    for line in text.splitlines():
        match = LINE.match(line)
        if match:
            rows.append((int(match.group(1), 16), match.group(3).strip()))
    return rows


def stub_targets(rows):
    stubs = {}
    for index, (address, text) in enumerate(rows):
        m = DIRECT.search(text)
        if not m or m.group(1) != 'jmp' or not text.startswith('jmp'):
            continue
        before = rows[index - 1][1] if index > 0 else 'int3'
        after = rows[index + 1][1] if index + 1 < len(rows) else 'int3'
        if (before.startswith('int3') or before.startswith('jmp')) and (after.startswith('int3') or after.startswith('jmp')):
            stubs[address] = int(m.group(2), 16)
    return stubs


def function_start(rows, index):
    for back in range(index - 1, max(-1, index - 120), -1):
        if BOUNDARY.search(rows[back][1]):
            for forward in range(back + 1, index + 1):
                if not re.search(r'\bint3\b|\bnop\b', rows[forward][1]):
                    return rows[forward][0]
            return rows[index][0]
    return None


def find_wrapper(rows, name_address):
    stored = None
    for index, (address, text) in enumerate(rows):
        target = RIP_TARGET.search(text)
        if text.startswith('lea') and target and int(target.group(1), 16) == name_address:
            for _, after in rows[index + 1:index + 8]:
                m = re.search(r'mov\s+QWORD PTR \[rip\+0x[0-9a-f]+\],rax\s+# 0x([0-9a-f]+)', after)
                if m:
                    stored = int(m.group(1), 16)
                    break
            if stored is not None:
                break
    if stored is None:
        return None
    for index, (address, text) in enumerate(rows):
        target = RIP_TARGET.search(text)
        if text.startswith('mov') and 'rax,QWORD PTR [rip' in text and target and int(target.group(1), 16) == stored:
            return function_start(rows, index)
    return None


def scan(rows, wrapper, guid_at):
    stubs = stub_targets(rows)
    wrapper_names = {wrapper} | {stub for stub, target in stubs.items() if target == wrapper}
    callers = defaultdict(int)
    for address, text in rows:
        m = DIRECT.search(text)
        if not m:
            continue
        target = int(m.group(2), 16)
        if address in stubs:
            continue
        callers[stubs.get(target, target)] += 1
    thunks = []
    for index, (address, text) in enumerate(rows):
        m = DIRECT.search(text)
        if not m or m.group(1) != 'call' or int(m.group(2), 16) not in wrapper_names:
            continue
        registers = {}
        for _, before in rows[max(0, index - 14):index]:
            lea = GUID_LEA.search(before)
            if lea:
                registers[lea.group(1)] = int(lea.group(2), 16)
            copy = COPY.search(before)
            if copy and copy.group(2) in registers:
                registers[copy.group(1)] = registers[copy.group(2)]
        slot = None
        shape = []
        for _, after in rows[index + 1:index + 60]:
            s = SLOT.search(after)
            if s:
                slot = int(s.group(1), 16) // 8
                break
            a = ARGUMENT.search(after)
            if a:
                shape.append(a.group(1))
            shape.extend(f"{size.lower()}@{offset}" for size, offset in STACK.findall(after))
            if re.search(r'\bret\b', after):
                break
        if 'rcx' not in registers or 'rdx' not in registers or slot is None:
            continue
        start = function_start(rows, index)
        thunks.append({
            "provider": guid_at(registers['rcx']),
            "interface": guid_at(registers['rdx']),
            "slot": slot,
            "thunk": start,
            "callers": callers.get(start, 0),
            "arguments": list(dict.fromkeys(shape)),
        })
    return thunks


class Image:
    def __init__(self, data):
        self.data = data
        pe = struct.unpack_from('<I', data, 0x3c)[0]
        if data[pe:pe + 4] != b'PE\0\0':
            raise ValueError("not a PE image")
        sections = struct.unpack_from('<H', data, pe + 6)[0]
        optional = struct.unpack_from('<H', data, pe + 20)[0]
        if struct.unpack_from('<H', data, pe + 24)[0] != 0x20b:
            raise ValueError("not a PE32+ image")
        self.base = struct.unpack_from('<Q', data, pe + 24 + 24)[0]
        self.sections = []
        for i in range(sections):
            offset = pe + 24 + optional + i * 40
            virtual_size, virtual_address, raw_size, raw_address = struct.unpack_from('<IIII', data, offset + 8)
            self.sections.append((virtual_address, virtual_size, raw_address, raw_size))

    def read(self, address, size):
        rva = address - self.base
        for va, vs, ra, rs in self.sections:
            if va <= rva < va + max(vs, rs) and rva - va + size <= rs:
                return self.data[ra + rva - va:ra + rva - va + size]
        return None

    def address_of(self, needle):
        offset = self.data.find(needle)
        while offset != -1:
            for va, vs, ra, rs in self.sections:
                if ra <= offset < ra + rs:
                    return self.base + va + offset - ra
            offset = self.data.find(needle, offset + 1)
        return None

    def guid(self, address):
        raw = self.read(address, 16)
        if raw is None:
            return None
        d1, d2, d3 = struct.unpack_from('<IHH', raw)
        return f"{d1:08x}-{d2:04x}-{d3:04x}-{raw[8]:02x}{raw[9]:02x}-{raw[10:].hex()}"


def summarize(thunks):
    interfaces = defaultdict(lambda: {"slots": set(), "used": {}})
    for thunk in thunks:
        entry = interfaces[(thunk["provider"], thunk["interface"])]
        entry["slots"].add(thunk["slot"])
        if thunk["callers"]:
            entry["used"][thunk["slot"]] = {"callers": thunk["callers"], "arguments": thunk["arguments"]}
    return [{"provider": provider, "interface": interface, "slots": sorted(entry["slots"]),
             "used": {str(slot): value for slot, value in sorted(entry["used"].items())}}
            for (provider, interface), entry in sorted(interfaces.items(), key=lambda item: -len(item[1]["slots"]))]


def main():
    parser = argparse.ArgumentParser(description="List the GDK runtime method tables a module's thunks reach through QueryApiImpl: GUID pairs, slots, argument shapes and how often the module calls each.")
    parser.add_argument("module", type=Path)
    parser.add_argument("--objdump", default=shutil.which("objdump") or "objdump")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    try:
        image = Image(args.module.read_bytes())
    except (OSError, ValueError, struct.error) as error:
        print(f"error: {args.module}: {error}", file=sys.stderr)
        return 2
    name = image.address_of(b'QueryApiImpl\0')
    if name is None:
        print(f"{args.module}: no QueryApiImpl lookup")
        return 0
    try:
        listing = subprocess.run([args.objdump, "-d", "-M", "intel", str(args.module)], capture_output=True, text=True, errors="replace", check=True).stdout
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"error: objdump failed: {error}", file=sys.stderr)
        return 2
    rows = parse_listing(listing)
    wrapper = find_wrapper(rows, name)
    if wrapper is None:
        print(f"{args.module}: QueryApiImpl is looked up but its call wrapper was not recognized")
        return 1
    thunks = scan(rows, wrapper, image.guid)
    interfaces = summarize(thunks)
    print(f"{args.module.name}: wrapper 0x{wrapper:x}, {len(thunks)} thunks, {len(interfaces)} interfaces, "
          f"{sum(len(i['used']) for i in interfaces)} methods called")
    for interface in interfaces:
        print(f"  {interface['provider']} {interface['interface']} slots {interface['slots'][0]}-{interface['slots'][-1]} ({len(interface['slots'])})")
        for slot, use in interface["used"].items():
            print(f"    slot {slot}: {use['callers']} call sites, arguments {' '.join(use['arguments']) or 'none'}")
    if args.json:
        args.json.write_text(json.dumps({"module": args.module.name, "wrapper": wrapper, "interfaces": interfaces}, indent=2), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
