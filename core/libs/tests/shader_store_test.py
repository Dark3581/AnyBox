import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path


def container(fill, size=64):
    data = bytearray(bytes([fill]) * size)
    data[0:4] = b"DXBC"
    struct.pack_into("<I", data, 24, size)
    return bytes(data)


LIBS = None


def run(command, env=None, expect=0):
    env = dict(env if env is not None else os.environ)
    env["PATH"] = str(LIBS) + os.pathsep + env.get("PATH", "")
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True, timeout=120, env=env)
    assert result.returncode == expect, (command, result.returncode, result.stdout, result.stderr)
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--libs", type=Path, required=True)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    global LIBS
    LIBS = args.libs
    shutil.rmtree(args.work, ignore_errors=True)
    carved, translated, store = args.work / "carved", args.work / "translated", args.work / "title" / "shaders"
    carved.mkdir(parents=True)
    translated.mkdir(parents=True)

    shaders = {"ps_a": (container(1), "ps"), "vs b": (container(2), "vs"), "ps_a_copy": (container(1), "ps"), "cs_untranslated": (container(3), "cs")}
    for name, (data, _) in shaders.items():
        (carved / f"{name}.dxbc").write_bytes(data)
    (carved / "index.json").write_text(json.dumps([{"name": n, "file": f"{n}.dxbc"} for n in shaders]), encoding="utf-8")
    translation = {}
    for name, (_, stage) in shaders.items():
        if name != "cs_untranslated":
            (translated / f"{name}.spv").write_bytes(b"\x03\x02\x23\x07" + shaders[name][0][:8])
            translation[name] = {"stage": stage, "wave64": False, **({"subgroup_size": 64} if stage == "ps" else {})}
    (translated / "index.json").write_text(json.dumps(translation), encoding="utf-8")

    built = run([sys.executable, args.tool, "build", carved, translated, store], expect=1)
    assert "2 modules stored" in built and "1 containers duplicate another" in built and "1 have no translation" in built, built
    index = (store / "modules.txt").read_text(encoding="utf-8").splitlines()
    assert len(index) == 2 and all(len(line.split("\t")) == 4 for line in index), index

    print(run([args.probe, "selftest"]))
    print(run([args.probe, "load", store, "ok", "-"]))
    found = [line.split("\t") for line in run([args.probe, "find", store, carved / "ps_a.dxbc", carved / "vs b.dxbc", carved / "ps_a_copy.dxbc", carved / "cs_untranslated.dxbc"]).splitlines()]
    assert [row[1] for row in found] == ["ps", "vs", "ps", "missing"], found
    assert [row[2] for row in found[:3]] == ["64", "0", "64"], found
    assert Path(found[0][3]).name == "ps_a.spv" and Path(found[1][3]).name == "vs b.spv" and found[2][3] == found[0][3], found
    assert found[0][4] == found[2][4] and found[0][4] in [line.split("\t")[0] for line in index], found
    for row in found[:3]:
        assert Path(row[3]).is_file(), row

    tool_lookup = run([sys.executable, args.tool, "lookup", store, carved / "vs b.dxbc"])
    assert tool_lookup.split(":")[0] == index[[l.split("\t")[1] for l in index].index("vs")].split("\t")[0], (tool_lookup, index)

    broken = args.work / "broken"
    broken.mkdir()
    (broken / "modules.txt").write_text(index[0] + "\nnot a line\n", encoding="utf-8")
    run([args.probe, "load", broken, "fail", "line 2 is not <sha256> <stage> <subgroup> <path>"])
    digest, _, _, path = index[0].split("\t")
    for line in (f"{digest}\tzz\t0\t{path}", f"{digest}\tps\t16\t{path}", f"{digest}\tps\t{path}"):
        (broken / "modules.txt").write_text(line + "\n", encoding="utf-8")
        run([args.probe, "load", broken, "fail", "line 1 is not <sha256> <stage> <subgroup> <path>"])
    run([args.probe, "load", args.work / "nowhere", "fail", "modules.txt not found"])

    env = dict(os.environ, ANYBOX_SHADER_STORE=str(store), ANYBOX_NO_REPORT="1", ANYBOX_TRACE="d3d12_x")
    title = run([args.probe, "title"], env=env)
    assert "title store 2" in title, title
    env.pop("ANYBOX_SHADER_STORE")
    assert "title store 0" in run([args.probe, "title"], env=env)
    print("PASS shader_store")


if __name__ == "__main__":
    main()
