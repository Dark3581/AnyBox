#include "PipelineCompiler.hpp"
#include "TitleLayout.hpp"
#include "VulkanDevice.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace abx::d3d12x;

constexpr int kNoDevice = 3;

const std::uint32_t kEmptyCompute[] = {
    0x07230203, 0x00010000, 0, 5, 0,
    0x00020011, 1,
    0x0003000E, 0, 1,
    0x0005000F, 5, 1, 0x6E69616D, 0,
    0x00060010, 1, 17, 1, 1, 1,
    0x00020013, 2,
    0x00030021, 3, 2,
    0x00050036, 2, 1, 0, 3,
    0x000200F8, 4,
    0x000100FD,
    0x00010038,
};

const std::uint32_t kOversizedPushConstants[] = {
    0x07230203, 0x00010000, 0x00070000, 0x00000010, 0x00000000, 0x00020011, 0x00000001, 0x0003000E,
    0x00000000, 0x00000001, 0x0005000F, 0x00000005, 0x00000001, 0x6E69616D, 0x00000000, 0x00060010,
    0x00000001, 0x00000011, 0x00000001, 0x00000001, 0x00000001, 0x00040047, 0x00000002, 0x00000006,
    0x00000004, 0x00050048, 0x00000003, 0x00000000, 0x00000023, 0x00000000, 0x00030047, 0x00000003,
    0x00000002, 0x00020013, 0x00000004, 0x00030021, 0x00000005, 0x00000004, 0x00040015, 0x00000006,
    0x00000020, 0x00000000, 0x0004002B, 0x00000006, 0x00000007, 0x00000028, 0x0004002B, 0x00000006,
    0x00000008, 0x00000027, 0x0004002B, 0x00000006, 0x00000009, 0x00000000, 0x0004001C, 0x00000002,
    0x00000006, 0x00000007, 0x0003001E, 0x00000003, 0x00000002, 0x00040020, 0x0000000A, 0x00000009,
    0x00000003, 0x00040020, 0x0000000B, 0x00000009, 0x00000006, 0x0004003B, 0x0000000A, 0x0000000C,
    0x00000009, 0x00050036, 0x00000004, 0x00000001, 0x00000000, 0x00000005, 0x000200F8, 0x0000000D,
    0x00060041, 0x0000000B, 0x0000000E, 0x0000000C, 0x00000009, 0x00000008, 0x0004003D, 0x00000006,
    0x0000000F, 0x0000000E, 0x000100FD, 0x00010038,
};

constexpr int kNoValidation = 4;

int Fail(const std::string& what) {
    std::printf("FAIL %s\n", what.c_str());
    return 1;
}

std::filesystem::path Utf8Path(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::vector<std::string> Split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::string part;
    std::istringstream stream(text);
    while (std::getline(stream, part, separator)) {
        if (!part.empty()) {
            parts.push_back(part);
        }
    }
    return parts;
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

std::unique_ptr<VulkanDevice> OpenDevice(const DeviceRequirements& requirements, int& status) {
    std::string error;
    std::unique_ptr<VulkanDevice> device = VulkanDevice::Create(requirements, error);
    if (!device) {
        std::printf("SKIP no usable Vulkan device: %s\n", error.c_str());
        status = kNoDevice;
        return nullptr;
    }
    std::printf("device %s, Vulkan %s, subgroup %u-%u\n", device->Gpu().name.c_str(), VersionText(device->Properties().apiVersion).c_str(),
                device->Gpu().subgroup.minSize, device->Gpu().subgroup.maxSize);
    std::string features;
    for (const std::string& feature : device->EnabledFeatures()) {
        features += (features.empty() ? "" : ",") + feature;
    }
    std::printf("enabled %s\n", features.c_str());
    return device;
}

int SelfTest(const std::filesystem::path& work) {
    int status = 0;
    std::string error;
    DeviceRequirements unknown;
    unknown.features.insert("notAFeature");
    if (VulkanDevice::Create(unknown, error) || error != "unknown shader feature notAFeature") return Fail("unknown feature: " + error);
    const std::unique_ptr<VulkanDevice> device = OpenDevice({}, status);
    if (!device) return status;

    std::filesystem::create_directories(work);
    const std::filesystem::path module = work / "empty.spv";
    std::ofstream(module, std::ios::binary).write(reinterpret_cast<const char*>(kEmptyCompute), sizeof(kEmptyCompute));
    const std::filesystem::path broken = work / "broken.spv";
    std::ofstream(broken, std::ios::binary).write("not spirv", 9);
    std::string readError;
    if (ReadSpirv(broken, readError) || readError.find("whole number") == std::string::npos) return Fail("odd-sized SPIR-V: " + readError);

    const std::filesystem::path cacheFile = work / "cache.bin";
    std::filesystem::remove(cacheFile);
    std::size_t written = 0;
    {
        PipelineCache cache(*device, cacheFile);
        if (!cache.Valid() || cache.SeedBytes() != 0 || !cache.Note().empty()) return Fail("new cache");
        ComputePipeline pipeline;
        const CompileOutcome outcome = CompileCompute(*device, cache.Handle(), {"empty", module, 0}, &pipeline);
        if (!outcome.ok || pipeline.Handle() == VK_NULL_HANDLE) return Fail("empty compute shader: " + outcome.error);
        const CompileOutcome missing = CompileCompute(*device, cache.Handle(), {"missing", work / "missing.spv", 0}, nullptr);
        if (missing.ok || missing.error.find("cannot read") == std::string::npos) return Fail("missing module");
        if (!cache.Save(written, error) || written < sizeof(VkPipelineCacheHeaderVersionOne)) return Fail("cache save: " + error);
    }
    {
        PipelineCache cache(*device, cacheFile);
        if (cache.SeedBytes() != written || !cache.Note().empty()) return Fail("cache reload");
    }
    std::vector<char> data(written);
    std::ifstream(cacheFile, std::ios::binary).read(data.data(), static_cast<std::streamsize>(data.size()));
    const VkPhysicalDeviceProperties& properties = device->Properties();
    if (!CacheHeaderMatches(data, properties.vendorID, properties.deviceID, properties.pipelineCacheUUID)) return Fail("own cache header");
    data[16] = static_cast<char>(data[16] ^ 0xFF);
    std::ofstream(cacheFile, std::ios::binary | std::ios::trunc).write(data.data(), static_cast<std::streamsize>(data.size()));
    {
        PipelineCache cache(*device, cacheFile);
        if (cache.SeedBytes() != 0 || cache.Note().find("another GPU") == std::string::npos) return Fail("foreign cache");
    }
    std::vector<ComputeJob> jobs;
    for (int index = 0; index < 6; ++index) {
        jobs.push_back({"empty" + std::to_string(index), module, 0});
    }
    jobs.push_back({"missing", work / "missing.spv", 0});
    PipelineCache batchCache(*device, {});
    for (const std::uintmax_t exclusiveBytes : {std::uintmax_t{64}, std::uintmax_t{1} << 20}) {
        BatchOptions options;
        options.workers = 3;
        options.exclusiveBytes = exclusiveBytes;
        std::size_t reported = 0;
        options.done = [&reported](const CompileOutcome&) { ++reported; };
        const std::vector<CompileOutcome> outcomes = CompileComputeBatch(*device, batchCache, jobs, options);
        std::size_t ok = 0;
        for (const CompileOutcome& outcome : outcomes) ok += outcome.ok;
        if (ok != 6 || reported != jobs.size() || outcomes.back().ok) return Fail("batch with exclusive size " + std::to_string(exclusiveBytes));
    }
    std::puts("PASS selftest");
    return 0;
}

int ValidationSelfTest(const std::filesystem::path& work) {
    DeviceRequirements requirements;
    requirements.validation = true;
    std::string error;
    const std::unique_ptr<VulkanDevice> device = VulkanDevice::Create(requirements, error);
    if (!device) {
        std::printf("SKIP %s\n", error.c_str());
        return error == "the Khronos validation layer is not installed" ? kNoValidation : kNoDevice;
    }
    std::filesystem::create_directories(work);
    const std::filesystem::path empty = work / "empty.spv";
    const std::filesystem::path oversized = work / "push160.spv";
    std::ofstream(empty, std::ios::binary).write(reinterpret_cast<const char*>(kEmptyCompute), sizeof(kEmptyCompute));
    std::ofstream(oversized, std::ios::binary).write(reinterpret_cast<const char*>(kOversizedPushConstants), sizeof(kOversizedPushConstants));
    PipelineCache cache(*device, {});
    if (!CompileCompute(*device, cache.Handle(), {"empty", empty, 0}, nullptr).ok || device->ValidationErrors() != 0) return Fail("a valid shader raised validation errors");
    CompileCompute(*device, cache.Handle(), {"push160", oversized, 0}, nullptr);
    if (device->ValidationErrors() == 0) return Fail("a 160-byte push constant block passed validation against the 144-byte layout");
    std::printf("caught: %.200s\n", device->ValidationMessages().front().c_str());
    std::puts("PASS validation");
    return 0;
}

int Compile(int argc, char** argv) {
    const std::string jobsFile = FindOption(argc, argv, "--jobs-file", "");
    if (jobsFile.empty()) return Fail("--jobs-file is required");
    DeviceRequirements requirements;
    for (const std::string& feature : Split(FindOption(argc, argv, "--features", ""), ',')) requirements.features.insert(feature);
    requirements.maintenance8 = HasFlag(argc, argv, "--maintenance8");
    requirements.validation = HasFlag(argc, argv, "--validate");
    std::vector<ComputeJob> jobs;
    std::ifstream list(Utf8Path(jobsFile), std::ios::binary);
    std::string line;
    while (std::getline(list, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> fields = Split(line, '\t');
        if (fields.size() != 3) return Fail("job line is not <name> <subgroup size> <path>: " + line);
        jobs.push_back({fields[0], Utf8Path(fields[2]), static_cast<std::uint32_t>(std::strtoul(fields[1].c_str(), nullptr, 10))});
    }
    int status = 0;
    const std::unique_ptr<VulkanDevice> device = OpenDevice(requirements, status);
    if (!device) return requirements.validation ? kNoValidation : status;
    PipelineCache cache(*device, Utf8Path(FindOption(argc, argv, "--cache", "")));
    if (!cache.Valid()) return Fail("vkCreatePipelineCache failed");
    if (!cache.Note().empty()) std::printf("cache: %s\n", cache.Note().c_str());
    BatchOptions options;
    options.workers = static_cast<unsigned>(std::strtoul(FindOption(argc, argv, "--workers", "1").c_str(), nullptr, 10));
    options.exclusiveBytes = std::strtoull(FindOption(argc, argv, "--exclusive-bytes", "4194304").c_str(), nullptr, 10);
    options.done = [](const CompileOutcome& outcome) {
        if (outcome.ok) {
            std::printf("OK\t%s\t%.3f\t%s\n", outcome.name.c_str(), outcome.seconds, outcome.subgroupForced ? "forced" : outcome.subgroupRequested ? "unforced" : "any");
        } else {
            std::printf("FAIL\t%s\t%s\n", outcome.name.c_str(), outcome.error.c_str());
        }
        std::fflush(stdout);
    };
    const std::vector<CompileOutcome> outcomes = CompileComputeBatch(*device, cache, jobs, options);
    std::size_t ok = 0, forced = 0, unforced = 0;
    double seconds = 0.0;
    for (const CompileOutcome& outcome : outcomes) {
        ok += outcome.ok;
        forced += outcome.ok && outcome.subgroupForced;
        unforced += outcome.ok && outcome.subgroupRequested && !outcome.subgroupForced;
        seconds += outcome.seconds;
    }
    std::size_t written = 0;
    std::string error;
    const bool saved = FindOption(argc, argv, "--cache", "").empty() || cache.Save(written, error);
    std::printf("summary ok=%zu failed=%zu forced=%zu unforced=%zu seconds=%.1f seed=%zu saved=%zu\n", ok, outcomes.size() - ok, forced, unforced, seconds,
                cache.SeedBytes(), written);
    if (!saved) return Fail("cache save: " + error);
    if (requirements.validation) {
        std::printf("validation errors=%zu\n", device->ValidationErrors());
        for (const std::string& message : device->ValidationMessages()) std::printf("  %.400s\n", message.c_str());
        if (device->ValidationErrors() != 0) return 1;
    }
    return ok == outcomes.size() ? 0 : 1;
}

}

int main(int argc, char** argv) {
    if (argc < 2) return Fail("usage: anybox-pipeline-probe layout | selftest <work folder> | validation <work folder> | compile --jobs-file <file> [--features a,b] [--maintenance8] [--validate] [--cache <file>] [--workers N] [--exclusive-bytes N]");
    const std::string command = argv[1];
    if (command == "layout") {
        std::fputs(TitleLayoutJson().c_str(), stdout);
        return 0;
    }
    if (command == "selftest" && argc == 3) return SelfTest(Utf8Path(argv[2]));
    if (command == "validation" && argc == 3) return ValidationSelfTest(Utf8Path(argv[2]));
    if (command == "compile") return Compile(argc, argv);
    return Fail("unknown command");
}
