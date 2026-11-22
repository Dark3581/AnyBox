#pragma once

#include "VulkanDevice.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace abx::d3d12x {

std::optional<std::vector<std::uint32_t>> ReadSpirv(const std::filesystem::path& path, std::string& error);
bool CacheHeaderMatches(const std::vector<char>& data, std::uint32_t vendorId, std::uint32_t deviceId, const std::uint8_t (&uuid)[VK_UUID_SIZE]);

class PipelineCache {
public:
    PipelineCache(const VulkanDevice& device, std::filesystem::path file);
    ~PipelineCache();
    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

    bool Valid() const { return cache_ != VK_NULL_HANDLE; }
    VkPipelineCache Handle() const { return cache_; }
    std::size_t SeedBytes() const { return seedBytes_; }
    const std::string& Note() const { return note_; }
    bool Save(std::size_t& written, std::string& error) const;

private:
    const VulkanDevice& device_;
    std::filesystem::path file_;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    std::size_t seedBytes_ = 0;
    std::string note_;
};

class ComputePipeline {
public:
    ComputePipeline() = default;
    ComputePipeline(const VulkanDevice& device, VkPipeline pipeline) : device_(&device), pipeline_(pipeline) {}
    ~ComputePipeline() { Reset(); }
    ComputePipeline(ComputePipeline&& other) noexcept : device_(other.device_), pipeline_(other.pipeline_) { other.pipeline_ = VK_NULL_HANDLE; }
    ComputePipeline& operator=(ComputePipeline&& other) noexcept;
    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    VkPipeline Handle() const { return pipeline_; }
    void Reset();

private:
    const VulkanDevice* device_ = nullptr;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

struct ComputeJob {
    std::string name;
    std::filesystem::path module;
    std::uint32_t subgroupSize = 0;
};

struct CompileOutcome {
    std::string name;
    bool ok = false;
    VkResult result = VK_SUCCESS;
    std::string error;
    bool subgroupRequested = false;
    bool subgroupForced = false;
    double seconds = 0.0;
    std::uintmax_t bytes = 0;
};

CompileOutcome CompileCompute(const VulkanDevice& device, VkPipelineCache cache, const ComputeJob& job, ComputePipeline* keep);

struct BatchOptions {
    unsigned workers = 1;
    std::uintmax_t exclusiveBytes = 4u << 20;
    std::function<void(const CompileOutcome&)> done;
};

std::vector<CompileOutcome> CompileComputeBatch(const VulkanDevice& device, const PipelineCache& cache, const std::vector<ComputeJob>& jobs, const BatchOptions& options);

}
