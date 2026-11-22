#include "PipelineCompiler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>

namespace abx::d3d12x {

namespace {

constexpr std::uint32_t kSpirvMagic = 0x07230203;

}

std::optional<std::vector<char>> ReadWholeFile(const std::filesystem::path& path, std::size_t pieceBytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || pieceBytes == 0) {
        return std::nullopt;
    }
    const std::streamoff size = file.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    std::vector<char> data(static_cast<std::size_t>(size));
    file.seekg(0);
    for (std::size_t done = 0; done < data.size();) {
        const std::size_t piece = std::min(pieceBytes, data.size() - done);
        if (!file.read(data.data() + done, static_cast<std::streamsize>(piece)) || file.gcount() != static_cast<std::streamsize>(piece)) {
            return std::nullopt;
        }
        done += piece;
    }
    return data;
}

bool WriteWholeFile(const std::filesystem::path& path, const std::vector<char>& data, std::size_t pieceBytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || pieceBytes == 0) {
        return false;
    }
    for (std::size_t done = 0; done < data.size();) {
        const std::size_t piece = std::min(pieceBytes, data.size() - done);
        if (!out.write(data.data() + done, static_cast<std::streamsize>(piece))) {
            return false;
        }
        done += piece;
    }
    out.close();
    return static_cast<bool>(out);
}

std::optional<std::vector<std::uint32_t>> ReadSpirv(const std::filesystem::path& path, std::string& error) {
    const std::optional<std::vector<char>> data = ReadWholeFile(path);
    if (!data) {
        error = "cannot read " + path.string();
        return std::nullopt;
    }
    if (data->size() < 20 || data->size() % 4 != 0) {
        error = path.string() + " is not a whole number of SPIR-V words";
        return std::nullopt;
    }
    std::vector<std::uint32_t> words(data->size() / 4);
    std::memcpy(words.data(), data->data(), data->size());
    if (words[0] != kSpirvMagic) {
        error = path.string() + " has no SPIR-V magic";
        return std::nullopt;
    }
    return words;
}

bool CacheHeaderMatches(const std::vector<char>& data, std::uint32_t vendorId, std::uint32_t deviceId, const std::uint8_t (&uuid)[VK_UUID_SIZE]) {
    VkPipelineCacheHeaderVersionOne header{};
    if (data.size() < sizeof(header)) {
        return false;
    }
    std::memcpy(&header, data.data(), sizeof(header));
    return header.headerSize >= sizeof(header) && header.headerSize <= data.size() && header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
           header.vendorID == vendorId && header.deviceID == deviceId && std::memcmp(header.pipelineCacheUUID, uuid, VK_UUID_SIZE) == 0;
}

PipelineCache::PipelineCache(const VulkanDevice& device, std::filesystem::path file) : device_(device), file_(std::move(file)) {
    std::vector<char> seed;
    if (!file_.empty()) {
        std::error_code ignored;
        if (std::filesystem::exists(file_, ignored)) {
            const std::optional<std::vector<char>> data = ReadWholeFile(file_);
            const VkPhysicalDeviceProperties& properties = device_.Properties();
            if (!data) {
                note_ = "the cache file could not be read; starting empty";
            } else if (!CacheHeaderMatches(*data, properties.vendorID, properties.deviceID, properties.pipelineCacheUUID)) {
                note_ = "the cache file is from another GPU or driver; starting empty";
            } else {
                seed = *data;
            }
        }
    }
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = seed.size();
    info.pInitialData = seed.empty() ? nullptr : seed.data();
    if (device_.Vk().createPipelineCache(device_.Handle(), &info, nullptr, &cache_) != VK_SUCCESS) {
        cache_ = VK_NULL_HANDLE;
        if (!seed.empty()) {
            note_ = "the driver refused the cache file; starting empty";
            info.initialDataSize = 0;
            info.pInitialData = nullptr;
            if (device_.Vk().createPipelineCache(device_.Handle(), &info, nullptr, &cache_) != VK_SUCCESS) {
                cache_ = VK_NULL_HANDLE;
            }
        }
        return;
    }
    seedBytes_ = seed.size();
}

PipelineCache::~PipelineCache() {
    if (cache_ != VK_NULL_HANDLE) {
        device_.Vk().destroyPipelineCache(device_.Handle(), cache_, nullptr);
    }
}

bool PipelineCache::Save(std::size_t& written, std::string& error) const {
    written = 0;
    if (cache_ == VK_NULL_HANDLE || file_.empty()) {
        error = "no cache to save";
        return false;
    }
    std::vector<char> data;
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::size_t size = 0;
        if (device_.Vk().getPipelineCacheData(device_.Handle(), cache_, &size, nullptr) != VK_SUCCESS) {
            error = "vkGetPipelineCacheData failed";
            return false;
        }
        data.resize(size);
        const VkResult result = device_.Vk().getPipelineCacheData(device_.Handle(), cache_, &size, data.data());
        if (result == VK_SUCCESS) {
            data.resize(size);
            break;
        }
        if (result != VK_INCOMPLETE || attempt == 3) {
            error = "vkGetPipelineCacheData failed with " + std::to_string(static_cast<int>(result));
            return false;
        }
    }
    std::filesystem::path partial = file_;
    partial += L".partial";
    if (!WriteWholeFile(partial, data)) {
        error = "cannot write " + partial.string();
        return false;
    }
    if (MoveFileExW(partial.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        error = "cannot replace " + file_.string() + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    written = data.size();
    return true;
}

ComputePipeline& ComputePipeline::operator=(ComputePipeline&& other) noexcept {
    if (this != &other) {
        Reset();
        device_ = other.device_;
        pipeline_ = other.pipeline_;
        other.pipeline_ = VK_NULL_HANDLE;
    }
    return *this;
}

void ComputePipeline::Reset() {
    if (pipeline_ != VK_NULL_HANDLE && device_ != nullptr) {
        device_->Vk().destroyPipeline(device_->Handle(), pipeline_, nullptr);
    }
    pipeline_ = VK_NULL_HANDLE;
}

CompileOutcome CompileCompute(const VulkanDevice& device, VkPipelineCache cache, const ComputeJob& job, ComputePipeline* keep) {
    CompileOutcome outcome;
    outcome.name = job.name;
    std::error_code sizeError;
    outcome.bytes = std::filesystem::file_size(job.module, sizeError);
    const std::optional<std::vector<std::uint32_t>> words = ReadSpirv(job.module, outcome.error);
    if (!words) {
        return outcome;
    }
    const auto start = std::chrono::steady_clock::now();
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = words->size() * sizeof(std::uint32_t);
    moduleInfo.pCode = words->data();
    VkShaderModule module = VK_NULL_HANDLE;
    outcome.result = device.Vk().createShaderModule(device.Handle(), &moduleInfo, nullptr, &module);
    if (outcome.result != VK_SUCCESS) {
        outcome.error = "vkCreateShaderModule failed with " + std::to_string(static_cast<int>(outcome.result));
        return outcome;
    }
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    subgroup.requiredSubgroupSize = job.subgroupSize;
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = device.TitlePipelineLayout();
    outcome.subgroupRequested = job.subgroupSize != 0;
    if (outcome.subgroupRequested && device.CanRequireSubgroupSize(job.subgroupSize, VK_SHADER_STAGE_COMPUTE_BIT)) {
        pipelineInfo.stage.pNext = &subgroup;
        outcome.subgroupForced = true;
    }
    VkPipeline pipeline = VK_NULL_HANDLE;
    outcome.result = device.Vk().createComputePipelines(device.Handle(), cache, 1, &pipelineInfo, nullptr, &pipeline);
    device.Vk().destroyShaderModule(device.Handle(), module, nullptr);
    outcome.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (outcome.result != VK_SUCCESS) {
        outcome.error = "vkCreateComputePipelines failed with " + std::to_string(static_cast<int>(outcome.result));
        return outcome;
    }
    ComputePipeline owned(device, pipeline);
    if (keep != nullptr) {
        *keep = std::move(owned);
    }
    outcome.ok = true;
    return outcome;
}

std::vector<CompileOutcome> CompileComputeBatch(const VulkanDevice& device, const PipelineCache& cache, const std::vector<ComputeJob>& jobs, const BatchOptions& options) {
    std::vector<CompileOutcome> outcomes(jobs.size());
    std::vector<std::size_t> shared, exclusive;
    for (std::size_t index = 0; index < jobs.size(); ++index) {
        std::error_code ignored;
        const std::uintmax_t size = std::filesystem::file_size(jobs[index].module, ignored);
        (size > options.exclusiveBytes && !ignored ? exclusive : shared).push_back(index);
    }
    std::mutex reportLock;
    const auto run = [&](std::size_t index) {
        outcomes[index] = CompileCompute(device, cache.Handle(), jobs[index], nullptr);
        if (options.done) {
            std::lock_guard<std::mutex> guard(reportLock);
            options.done(outcomes[index]);
        }
    };
    std::atomic<std::size_t> next{0};
    const unsigned workers = std::max(1u, std::min<unsigned>(options.workers, static_cast<unsigned>(std::max<std::size_t>(shared.size(), 1))));
    std::vector<std::thread> pool;
    for (unsigned worker = 0; worker < workers; ++worker) {
        pool.emplace_back([&] {
            for (std::size_t slot = next++; slot < shared.size(); slot = next++) {
                run(shared[slot]);
            }
        });
    }
    for (std::thread& thread : pool) {
        thread.join();
    }
    for (const std::size_t index : exclusive) {
        run(index);
    }
    return outcomes;
}

}
