import argparse
import json
import os
import random
import shutil
import struct
import subprocess
import sys
from pathlib import Path


def container(fill, size=48):
    data = bytearray(bytes([fill]) * size)
    data[0:4] = b"DXBC"
    struct.pack_into("<I", data, 24, size)
    return bytes(data)


def empty_compute(local_x):
    words = [0x07230203, 0x00010000, 0, 5, 0,
             0x00020011, 1,
             0x0003000E, 0, 1,
             0x0005000F, 5, 1, 0x6E69616D, 0,
             0x00060010, 1, 17, local_x, 1, 1,
             0x00020013, 2,
             0x00030021, 3, 2,
             0x00050036, 2, 1, 0, 3,
             0x000200F8, 4,
             0x000100FD,
             0x00010038]
    return struct.pack(f"<{len(words)}I", *words)


def run(command, env, expect=0):
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True, env=env, timeout=600)
    print(result.stdout.strip())
    assert result.returncode == expect, (command, result.returncode, result.stdout[-2000:], result.stderr[-2000:])
    return result.stdout


def rows(output, containers):
    found = {}
    for line in output.splitlines():
        fields = line.split("\t")
        if len(fields) == 6 and fields[0] in containers:
            found.setdefault(fields[0], []).append({"origin": fields[1], "hit": fields[2] == "1", "bytes": int(fields[3]), "error": fields[5]})
    return found


def stats(output):
    line = next(line for line in output.splitlines() if line.startswith("stats "))
    return {key: int(value) for key, value in (field.split("=") for field in line.split()[1:])}


def check_settings(args, env):
    config = args.work / "anybox.cfg"
    config.write_text("# test values, not a console's\n"
                      "d3d12_x.descriptor_size.shader_resource = 32\n"
                      "d3d12_x.descriptor_size.sampler=0x10   # hexadecimal\n"
                      "\n"
                      "not a setting\n"
                      "d3d12_x.descriptor_size.render_target = 64\n"
                      "d3d12_x.descriptor_size.render_target = 128\n", encoding="utf-8")
    settings_env = dict(env, ANYBOX_CONFIG=str(config), ANYBOX_SET="d3d12_x.descriptor_size.depth_stencil=48; other.key = x; d3d12_x.vulkan_device = rtx")
    checked = subprocess.run([str(args.probe), "settings-selftest"], capture_output=True, text=True, env=settings_env, timeout=60)
    assert checked.returncode == 0, (checked.stdout, checked.stderr)
    assert "sizes 32 16 128 48" in checked.stdout and "other x missing null" in checked.stdout and "vulkan device rtx" in checked.stdout, checked.stdout
    log = checked.stderr
    assert "read 3 setting(s) from" in log, log
    assert "anybox.cfg:5: ignored, not a lower-case key = value" in log, log
    assert "anybox.cfg:7: d3d12_x.descriptor_size.render_target replaces the value from anybox.cfg:6" in log, log
    assert "setting d3d12_x.descriptor_size.render_target = 128 (from anybox.cfg:7)" in log, log
    assert "setting d3d12_x.descriptor_size.depth_stencil = 48 (from ANYBOX_SET)" in log, log
    assert log.count("setting other.key") == 1, log
    unset = subprocess.run([str(args.probe), "settings-selftest"], capture_output=True, text=True, env=dict(env, ANYBOX_CONFIG=str(args.work / "missing.cfg")), timeout=60)
    assert "sizes unset unset unset unset" in unset.stdout and "cannot be read" in unset.stderr, (unset.stdout, unset.stderr)
    broken = subprocess.run([str(args.probe), "settings-selftest"], capture_output=True, text=True, env=dict(settings_env, ANYBOX_SET="d3d12_x.descriptor_size.sampler=abc"), timeout=60)
    assert "error\tsetting d3d12_x.descriptor_size.sampler = abc is not a descriptor size in bytes" in broken.stdout, broken.stdout
    print("settings ok")


def check_graphics(args, env):
    shaders = args.work / "test_shaders"
    run([args.probe, "dump-test-shaders", shaders], env)
    carved, translated, store = args.work / "gcarved", args.work / "gtranslated", args.work / "gstore"
    carved.mkdir(parents=True)
    translated.mkdir(parents=True)
    inputs_signature = [{"register": 0, "semantic": "POSITION", "semantic_index": 0, "mask": 7, "system_value": 0},
                        {"register": 1, "semantic": "COLOR", "semantic_index": 0, "mask": 15, "system_value": 0},
                        {"register": 3, "semantic": "BLENDINDICES", "semantic_index": 0, "mask": 15, "system_value": 0}]
    modules = {
        "vs_plain": (container(11), {"stage": "vs"}, "test_vertex.spv"),
        "vs_inputs": (container(12), {"stage": "vs", "input_signature": inputs_signature,
                                      "vertex_inputs": [{"location": 0, "type": "float"}, {"location": 1, "type": "float"}, {"location": 3, "type": "uint"}]}, "test_inputs.spv"),
        "ps_float": (container(13), {"stage": "ps"}, "test_float.spv"),
    }
    for name, (data, details, spirv) in modules.items():
        (carved / f"{name}.dxbc").write_bytes(data)
        shutil.copyfile(shaders / spirv, translated / f"{name}.spv")
    (carved / "index.json").write_text(json.dumps([{"name": n, "file": f"{n}.dxbc"} for n in modules]), encoding="utf-8")
    (translated / "index.json").write_text(json.dumps({n: d for n, (_, d, _) in modules.items()}), encoding="utf-8")
    run([sys.executable, args.tool, "build", carved, translated, store], env)

    def graphics(vertex, pixel, *extra):
        result = subprocess.run([str(part) for part in [args.probe, "graphics", store, carved / f"{vertex}.dxbc", carved / f"{pixel}.dxbc", *extra]],
                                capture_output=True, text=True, env=env, timeout=600)
        print(result.stdout.strip())
        assert result.returncode in (0, 3), (result.stdout[-2000:], result.stderr[-2000:])
        return result

    assert graphics("vs_inputs", "ps_float").stdout.startswith("error\tthe input layout has no element for POSITION0")
    assert graphics("ps_float", "ps_float").stdout.startswith("error\tthe shader store has stage ps for the vertex shader ")
    assert graphics("vs_plain", "vs_plain").stdout.startswith("error\tthe shader store has stage vs for the pixel shader ")
    cache = args.work / "graphics_cache.bin"
    first = graphics("vs_plain", "ps_float", "--cache", cache, "--save")
    if first.returncode == 3:
        print("SKIP graphics pipelines through the library: no usable Vulkan device")
        return
    assert "request 2 shaders, 0 attributes" in first.stdout, first.stdout
    counted = stats(first.stdout)
    assert counted["ready"] + counted["compiled"] == 2 and counted["refused"] == 0 and counted["failed"] == 0, counted
    cache_control = "pipeline cache control yes" in first.stdout
    again = graphics("vs_plain", "ps_float", "--cache", cache)
    if cache_control:
        assert stats(again.stdout) == {"ready": 2, "compiled": 0, "refused": 0, "failed": 0}, again.stdout
    with_inputs = graphics("vs_inputs", "ps_float", "--layout", "--cache", cache)
    assert "request 2 shaders, 3 attributes" in with_inputs.stdout, with_inputs.stdout
    counted = stats(with_inputs.stdout)
    assert counted["failed"] == 0 and counted["refused"] == 0 and counted["ready"] + counted["compiled"] == 2, counted
    limited = graphics("vs_inputs", "ps_float", "--layout", "--cache", args.work / "graphics_fresh.bin", "--limit", "16")
    counted = stats(limited.stdout)
    assert counted["compiled"] == 0 and counted["failed"] == 0 and counted["refused"] + counted["ready"] == 2, counted
    pipeline_objects(args, env, store, "graphics", carved / "vs_plain.dxbc", carved / "ps_float.dxbc")


def pipeline_objects(args, env, store, kind, *containers):
    command = [str(part) for part in [args.probe, "pso-objects", store, kind, *containers]]
    checked = subprocess.run(command + ["--validate"], capture_output=True, text=True, env=env, timeout=600)
    if checked.returncode == 4:
        checked = subprocess.run(command, capture_output=True, text=True, env=env, timeout=600)
    print(checked.stdout.strip())
    assert checked.returncode in (0, 3), (kind, checked.stdout[-3000:], checked.stderr[-2000:])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--libs", type=Path, required=True)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    shutil.rmtree(args.work, ignore_errors=True)
    carved, translated, store = args.work / "carved", args.work / "translated", args.work / "store"
    carved.mkdir(parents=True)
    translated.mkdir(parents=True)
    env = dict(os.environ, PATH=str(args.libs) + os.pathsep + os.environ.get("PATH", ""), ANYBOX_NO_REPORT="1")

    unique = random.SystemRandom().randrange(2, 1024)
    shaders = {
        "cs_a": (container(1), {"stage": "cs", "subgroup_size": None}, empty_compute(1)),
        "cs_b": (container(2), {"stage": "cs", "subgroup_size": 32}, empty_compute(2)),
        "cs_new": (container(3), {"stage": "cs", "subgroup_size": None}, empty_compute(unique)),
        "ps_x": (container(4), {"stage": "ps", "subgroup_size": 64}, b"\x03\x02\x23\x07" + bytes(16)),
    }
    for name, (data, details, spirv) in shaders.items():
        (carved / f"{name}.dxbc").write_bytes(data)
        (translated / f"{name}.spv").write_bytes(spirv)
    (carved / "index.json").write_text(json.dumps([{"name": n, "file": f"{n}.dxbc"} for n in shaders]), encoding="utf-8")
    (translated / "index.json").write_text(json.dumps({n: d for n, (_, d, _) in shaders.items()}), encoding="utf-8")
    run([sys.executable, args.tool, "build", carved, translated, store], env)
    pipeline_objects(args, env, store, "compute", carved / "cs_a.dxbc")
    (carved / "unknown.dxbc").write_bytes(container(9))
    (carved / "junk.bin").write_bytes(b"not a container at all, but long enough to read")

    def resolve(container_path, *extra):
        return run([args.probe, "resolve", store, container_path, *extra], env).strip().split("\t")

    resolved = resolve(carved / "cs_b.dxbc")
    assert resolved[0] == "ok" and len(resolved[1]) == 64 and resolved[2] == "32" and Path(resolved[3]).name == "cs_b.spv", resolved
    assert resolve(carved / "cs_a.dxbc", "--cached", "64")[:1] == ["ok"]
    assert resolve(carved / "cs_a.dxbc")[2] == "0"
    assert resolve(carved / "ps_x.dxbc")[1].startswith("the shader store has stage ps for compute shader ")
    assert resolve(carved / "unknown.dxbc")[1].endswith(" is not in the shader store")
    assert resolve(carved / "junk.bin")[1] == "the compute shader is not a DXBC container"
    assert resolve("none")[1] == "the description has no compute shader"
    assert resolve(carved / "cs_a.dxbc", "--flags", "1")[1] == "pipeline state flags 0x1 are not supported"
    assert resolve(carved / "cs_a.dxbc", "--node", "2")[1] == "node mask 0x2 names a GPU node other than the first"
    assert resolve(carved / "cs_a.dxbc", "--node", "1")[0] == "ok"

    assert "PASS graphics selftest" in run([args.probe, "graphics-selftest"], env)
    assert "PASS input selftest" in run([args.probe, "input-selftest"], env)
    assert "PASS states selftest" in run([args.probe, "states-selftest"], env)
    assert "PASS textures selftest" in run([args.probe, "textures-selftest"], env)
    assert "PASS views selftest" in run([args.probe, "views-selftest"], env)
    assert "PASS commands selftest" in run([args.probe, "commands-selftest"], env)
    assert "PASS draw selftest" in run([args.probe, "draw-selftest"], env)
    assert "PASS indirect selftest" in run([args.probe, "indirect-selftest"], env)
    assert "PASS buffers selftest" in run([args.probe, "buffers-selftest"], env)
    assert "PASS root signature selftest" in run([args.probe, "rootsig-selftest"], env)
    assert "PASS arena selftest" in run([args.probe, "arena-selftest"], env)
    check_settings(args, env)
    selection = subprocess.run([str(args.probe), "device-selection"], capture_output=True, text=True, env=env, timeout=120)
    print(selection.stdout.strip())
    assert selection.returncode in (0, 3) and "PASS device selection selftest" in selection.stdout, (selection.stdout, selection.stderr)
    for command in ("states-gpu", "textures-gpu", "views-gpu", "commands-gpu", "draw-gpu", "indirect-gpu", "planes-gpu", "fence-gpu", "resources-gpu", "commandlist-gpu", "descriptors-gpu"):
        checked = subprocess.run([str(args.probe), command, "--validate"], capture_output=True, text=True, env=env, timeout=600)
        if checked.returncode == 4:
            checked = subprocess.run([str(args.probe), command], capture_output=True, text=True, env=env, timeout=600)
        print(checked.stdout.strip())
        assert checked.returncode in (0, 3), (command, checked.stdout[-3000:], checked.stderr[-2000:])
    graphics = subprocess.run([str(args.probe), "graphics-gpu", "--validate"], capture_output=True, text=True, env=env, timeout=600)
    if graphics.returncode == 4:
        print("validation layer not installed: creating graphics pipelines without it")
        graphics = subprocess.run([str(args.probe), "graphics-gpu"], capture_output=True, text=True, env=env, timeout=600)
    print(graphics.stdout.strip())
    assert graphics.returncode in (0, 3), (graphics.stdout[-3000:], graphics.stderr[-2000:])

    names = [carved / "cs_a.dxbc", carved / "cs_b.dxbc", carved / "cs_a.dxbc", carved / "ps_x.dxbc", carved / "cs_a.dxbc", carved / "cs_b.dxbc"]
    listing = args.work / "containers.txt"
    listing.write_text("".join(f"{name}\n" for name in names), encoding="utf-8")
    cache = args.work / "cache.bin"
    command = [args.probe, "compute", store, listing, "--cache", cache, "--threads", "4", "--save"]
    probe = subprocess.run([str(part) for part in command], capture_output=True, text=True, env=env, timeout=600)
    print(probe.stdout.strip())
    if probe.returncode == 3:
        print("SKIP pipeline creation: no usable Vulkan device")
        print("PASS pipeline_state (resolution only)")
        return
    assert probe.returncode == 0, (probe.stdout[-2000:], probe.stderr[-2000:])
    keys = {str(name) for name in names}
    first = rows(probe.stdout, keys)
    for name in (carved / "cs_a.dxbc", carved / "cs_b.dxbc"):
        entries = first[str(name)]
        assert {entry["origin"] for entry in entries} <= {"ready", "compiled"} and len({entry["origin"] for entry in entries}) == 1, (name, entries)
        assert all(entry["bytes"] == len(shaders[name.stem][2]) for entry in entries), entries
    assert first[str(carved / "ps_x.dxbc")][0]["error"].startswith("the shader store has stage ps"), first
    counted = stats(probe.stdout)
    assert counted["ready"] + counted["compiled"] == 2 and counted["refused"] == 0 and counted["failed"] == 0, counted
    cache_control = "pipeline cache control yes" in probe.stdout

    second = run(command, env)
    if cache_control:
        assert stats(second) == {"ready": 2, "compiled": 0, "refused": 0, "failed": 0}, second

    fresh = args.work / "fresh.bin"
    limited = args.work / "limited.txt"
    limited.write_text(f"{carved / 'cs_new.dxbc'}\n{carved / 'cs_a.dxbc'}\n", encoding="utf-8")
    output = run([args.probe, "compute", store, limited, "--cache", fresh, "--limit", "16"], env)
    counted = stats(output)
    assert counted["compiled"] == 0 and counted["failed"] == 0 and counted["refused"] + counted["ready"] == 2, counted
    for entry in sum(rows(output, {str(carved / "cs_new.dxbc"), str(carved / "cs_a.dxbc")}).values(), []):
        if entry["origin"] == "refused":
            expected = "build the cache offline" if cache_control else "cannot tell whether the pipeline cache holds it"
            assert expected in entry["error"], entry
    print(f"over the limit with an empty cache: {counted['refused']} refused, {counted['ready']} ready from the driver's own cache")

    output = run([args.probe, "compute", store, limited, "--cache", fresh, "--limit", "100000"], env)
    assert stats(output)["failed"] == 0 and stats(output)["refused"] == 0, output
    check_graphics(args, env)
    print("PASS pipeline_state")


if __name__ == "__main__":
    main()
