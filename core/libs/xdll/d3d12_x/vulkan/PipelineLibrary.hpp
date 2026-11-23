#pragma once

#include "GraphicsPipeline.hpp"
#include "VulkanDevice.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace abx::d3d12x {

struct PipelineSource {
    std::string key;
    std::filesystem::path module;
    std::uint32_t subgroupSize = 0;
};

struct GraphicsShaderSource {
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_VERTEX_BIT;
    std::string key;
    std::filesystem::path module;
    std::uint32_t subgroupSize = 0;
};

struct GraphicsRequest {
    std::string key;
    std::vector<GraphicsShaderSource> shaders;
    GraphicsPipelineState state;
    VertexInput input;
};

enum class PipelineOrigin { Ready, Compiled, Refused, Failed };

const char* PipelineOriginName(PipelineOrigin origin);

struct PipelineEntry {
    VkPipeline pipeline = VK_NULL_HANDLE;
    PipelineOrigin origin = PipelineOrigin::Failed;
    bool driverCacheHit = false;
    bool subgroupForced = false;
    bool usesCodeBase = false;
    std::uintmax_t bytes = 0;
    double seconds = 0.0;
    std::string error;
};

struct PipelineLibraryStats {
    std::size_t ready = 0;
    std::size_t compiled = 0;
    std::size_t refused = 0;
    std::size_t failed = 0;
};

class PipelineLibrary {
public:
    PipelineLibrary(const VulkanDevice& device, VkPipelineCache cache, std::uintmax_t compileLimitBytes);
    ~PipelineLibrary();
    PipelineLibrary(const PipelineLibrary&) = delete;
    PipelineLibrary& operator=(const PipelineLibrary&) = delete;

    PipelineEntry Compute(const PipelineSource& source);
    PipelineEntry Graphics(const GraphicsRequest& request);
    PipelineLibraryStats Stats() const;
    std::uintmax_t CompileLimitBytes() const { return compileLimitBytes_; }

private:
    struct Slot {
        bool done = false;
        PipelineEntry entry;
    };

    PipelineEntry Obtain(const std::string& key, const std::function<PipelineEntry()>& create);
    PipelineEntry Create(const PipelineSource& source) const;
    PipelineEntry CreateGraphics(const GraphicsRequest& request) const;

    const VulkanDevice& device_;
    VkPipelineCache cache_;
    std::uintmax_t compileLimitBytes_;
    mutable std::mutex lock_;
    std::condition_variable finished_;
    std::unordered_map<std::string, std::unique_ptr<Slot>> slots_;
    PipelineLibraryStats stats_;
};

}
