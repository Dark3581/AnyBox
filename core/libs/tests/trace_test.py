import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def run(probe, libs, scenario, trace, log=None):
    env = dict(os.environ, ANYBOX_NO_REPORT="1")
    env.pop("ANYBOX_TRACE", None)
    env.pop("ANYBOX_LOG_FILE", None)
    if trace is not None:
        env["ANYBOX_TRACE"] = trace
    if log is not None:
        env["ANYBOX_LOG_FILE"] = str(log)
    result = subprocess.run([str(probe), str(libs), scenario], capture_output=True, text=True, env=env, timeout=60)
    assert result.returncode == 0, (scenario, trace, result.stdout, result.stderr)
    return result.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--libs", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    shutil.rmtree(args.work, ignore_errors=True)
    args.work.mkdir(parents=True)

    assert "XMemAllocDefault(" not in run(args.probe, args.libs, "xmem", None), "tracing is off by default"
    assert "XMemAllocDefault(" not in run(args.probe, args.libs, "xmem", ""), "an empty list traces nothing"
    assert "XMemAllocDefault(" not in run(args.probe, args.libs, "xmem", "d3d12_x"), "only the listed libraries are traced"

    traced = run(args.probe, args.libs, "xmem", "xmem")
    assert "[xmem] XMemAllocDefault(100, 0x5e50000) -> " in traced, traced
    assert "[xmem] XMemFreeDefault(" in traced and "[xmem] XMemVirtualQuery(" in traced, traced
    assert "[xmem] XMemGetWorkingSetStatistics(0x20000001, " in traced and ") -> 0" in traced, traced

    mixed = run(args.probe, args.libs, "rootsig", " gameinput , D3D12_X ")
    assert "[d3d12_x] D3D12SerializeRootSignature(" in mixed and "flags 0x3000001 -> 0x1, result 0x00000000" in mixed, mixed
    assert "[d3d12_x] D3D12CreateVersionedRootSignatureDeserializer(" in mixed, mixed

    log = args.work / "anybox.log"
    everything = run(args.probe, args.libs, "rootsig", "all", log)
    assert "[d3d12_x] D3D12SerializeVersionedRootSignature(" in everything, everything
    assert "[d3d12_x] D3D12SerializeVersionedRootSignature(" in log.read_text(encoding="utf-8"), "trace lines go to ANYBOX_LOG_FILE too"
    print("PASS trace")


if __name__ == "__main__":
    main()
