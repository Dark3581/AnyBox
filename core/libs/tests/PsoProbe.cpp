#include "PipelineCompiler.hpp"
#include "PipelineLibrary.hpp"
#include "PipelineState.hpp"
#include "ShaderStore.hpp"
#include "VulkanDevice.hpp"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace abx::d3d12x;

constexpr int kNoDevice = 3;

std::filesystem::path Utf8Path(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string FindOption(int argc, char** argv, const char* name, const char* fallback) {
    for (int index = 2; index + 1 < argc; ++index) {
        if (std::strcmp(argv[index], name) == 0) {
            return argv[index + 1];
        }
    }
    return fallback;
}

bool HasFlag(int argc, char** argv, const char* name) {
    for (int index = 2; index < argc; ++index) {
        if (std::strcmp(argv[index], name) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<unsigned char> ReadBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

bool LoadStore(const std::string& folder, ShaderStore& store) {
    std::string error;
    if (!store.Load(Utf8Path(folder).wstring(), error)) {
        std::printf("FAIL store: %s\n", error.c_str());
        return false;
    }
    return true;
}

D3D12_COMPUTE_PIPELINE_STATE_DESC Description(const std::vector<unsigned char>& bytecode) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.CS.pShaderBytecode = bytecode.empty() ? nullptr : bytecode.data();
    desc.CS.BytecodeLength = bytecode.size();
    return desc;
}

int Resolve(int argc, char** argv) {
    ShaderStore store;
    if (!LoadStore(argv[2], store)) return 1;
    std::vector<unsigned char> bytecode;
    if (std::strcmp(argv[3], "none") != 0) {
        bytecode = ReadBytes(Utf8Path(argv[3]));
        bytecode.resize(bytecode.size() + 5, 0);
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = Description(bytecode);
    desc.Flags = static_cast<D3D12_PIPELINE_STATE_FLAGS>(std::strtoul(FindOption(argc, argv, "--flags", "0").c_str(), nullptr, 0));
    desc.NodeMask = static_cast<UINT>(std::strtoul(FindOption(argc, argv, "--node", "0").c_str(), nullptr, 0));
    const std::vector<unsigned char> blob(std::strtoul(FindOption(argc, argv, "--cached", "0").c_str(), nullptr, 0), 0xCC);
    desc.CachedPSO.pCachedBlob = blob.empty() ? nullptr : blob.data();
    desc.CachedPSO.CachedBlobSizeInBytes = blob.size();
    std::string error;
    const std::optional<PipelineSource> source = ComputePipelineSource(desc, store, error);
    if (!source) {
        std::printf("error\t%s\n", error.c_str());
        return 0;
    }
    const std::u8string module = source->module.u8string();
    std::printf("ok\t%s\t%u\t%s\n", source->key.c_str(), source->subgroupSize, std::string(module.begin(), module.end()).c_str());
    return 0;
}

std::vector<std::string> Lines(const std::filesystem::path& path) {
    std::vector<std::string> lines;
    std::ifstream file(path, std::ios::binary);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

struct Row {
    PipelineEntry entry;
    std::string error;
};

int Compute(int argc, char** argv) {
    ShaderStore store;
    if (!LoadStore(argv[2], store)) return 1;
    const std::vector<std::string> containers = Lines(Utf8Path(argv[3]));
    if (containers.empty()) {
        std::puts("FAIL no containers listed");
        return 1;
    }
    DeviceRequirements requirements;
    std::istringstream features(FindOption(argc, argv, "--features", ""));
    for (std::string feature; std::getline(features, feature, ',');) {
        if (!feature.empty()) requirements.features.insert(feature);
    }
    requirements.maintenance8 = HasFlag(argc, argv, "--maintenance8");
    std::string error;
    const std::unique_ptr<VulkanDevice> device = VulkanDevice::Create(requirements, error);
    if (!device) {
        std::printf("SKIP no usable Vulkan device: %s\n", error.c_str());
        return kNoDevice;
    }
    std::printf("device %s, pipeline cache control %s\n", device->Gpu().name.c_str(), device->PipelineCacheControl() ? "yes" : "no");
    PipelineCache cache(*device, Utf8Path(FindOption(argc, argv, "--cache", "")));
    if (!cache.Valid()) {
        std::puts("FAIL pipeline cache");
        return 1;
    }
    std::printf("cache seeded with %zu bytes%s%s\n", cache.SeedBytes(), cache.Note().empty() ? "" : "; ", cache.Note().c_str());
    std::vector<Row> rows(containers.size());
    {
        PipelineLibrary library(*device, cache.Handle(), std::strtoull(FindOption(argc, argv, "--limit", "0").c_str(), nullptr, 10));
        std::atomic<std::size_t> next{0};
        const unsigned threads = static_cast<unsigned>(std::max(1ul, std::strtoul(FindOption(argc, argv, "--threads", "1").c_str(), nullptr, 10)));
        std::vector<std::thread> pool;
        for (unsigned worker = 0; worker < threads; ++worker) {
            pool.emplace_back([&] {
                for (std::size_t index = next++; index < containers.size(); index = next++) {
                    const std::vector<unsigned char> bytecode = ReadBytes(Utf8Path(containers[index]));
                    std::string reason;
                    const std::optional<PipelineSource> source = ComputePipelineSource(Description(bytecode), store, reason);
                    if (!source) {
                        rows[index].error = reason;
                        continue;
                    }
                    rows[index].entry = library.Compute(*source);
                    rows[index].error = rows[index].entry.error;
                }
            });
        }
        for (std::thread& thread : pool) thread.join();
        for (std::size_t index = 0; index < containers.size(); ++index) {
            const PipelineEntry& entry = rows[index].entry;
            std::printf("%s\t%s\t%d\t%llu\t%.3f\t%s\n", containers[index].c_str(), PipelineOriginName(entry.origin), entry.driverCacheHit ? 1 : 0,
                        static_cast<unsigned long long>(entry.bytes), entry.seconds, rows[index].error.c_str());
        }
        const PipelineLibraryStats stats = library.Stats();
        std::printf("stats ready=%zu compiled=%zu refused=%zu failed=%zu\n", stats.ready, stats.compiled, stats.refused, stats.failed);
    }
    if (HasFlag(argc, argv, "--save")) {
        std::size_t written = 0;
        if (!cache.Save(written, error)) {
            std::printf("FAIL cache save: %s\n", error.c_str());
            return 1;
        }
        std::printf("cache saved, %zu bytes\n", written);
    }
    return 0;
}

}

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "resolve") == 0) return Resolve(argc, argv);
    if (argc >= 4 && std::strcmp(argv[1], "compute") == 0) return Compute(argc, argv);
    std::puts("usage: resolve <store> <container|none> [--flags N] [--node N] [--cached N] | compute <store> <containers file> [--cache F] [--limit BYTES] [--threads N] [--features a,b] [--maintenance8] [--save]");
    return 2;
}
