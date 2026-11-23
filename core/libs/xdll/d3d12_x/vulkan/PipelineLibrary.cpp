#include "PipelineLibrary.hpp"
#include "PipelineCompiler.hpp"

#include <chrono>
#include <exception>
#include <optional>
#include <vector>

namespace abx::d3d12x {

const char* PipelineOriginName(PipelineOrigin origin) {
    switch (origin) {
    case PipelineOrigin::Ready:
        return "ready";
    case PipelineOrigin::Compiled:
        return "compiled";
    case PipelineOrigin::Refused:
        return "refused";
    case PipelineOrigin::Failed:
        return "failed";
    }
    return "unknown";
}

PipelineLibrary::PipelineLibrary(const VulkanDevice& device, VkPipelineCache cache, std::uintmax_t compileLimitBytes)
    : device_(device), cache_(cache), compileLimitBytes_(compileLimitBytes) {}

PipelineLibrary::~PipelineLibrary() {
    for (const auto& [key, slot] : slots_) {
        if (slot && slot->entry.pipeline != VK_NULL_HANDLE) {
            device_.Vk().destroyPipeline(device_.Handle(), slot->entry.pipeline, nullptr);
        }
    }
}

PipelineEntry PipelineLibrary::Compute(const PipelineSource& source) {
    if (source.key.empty()) {
        PipelineEntry entry;
        entry.error = "the pipeline has no key";
        return entry;
    }
    return Obtain("cs:" + source.key, [this, &source] { return Create(source); });
}

PipelineEntry PipelineLibrary::Graphics(const GraphicsRequest& request) {
    if (request.key.empty()) {
        PipelineEntry entry;
        entry.error = "the pipeline has no key";
        return entry;
    }
    return Obtain("gfx:" + request.key, [this, &request] { return CreateGraphics(request); });
}

PipelineEntry PipelineLibrary::Obtain(const std::string& key, const std::function<PipelineEntry()>& create) {
    std::unique_lock<std::mutex> guard(lock_);
    const auto [found, inserted] = slots_.try_emplace(key);
    if (!inserted) {
        Slot& slot = *found->second;
        finished_.wait(guard, [&slot] { return slot.done; });
        return slot.entry;
    }
    found->second = std::make_unique<Slot>();
    Slot& slot = *found->second;
    guard.unlock();
    PipelineEntry entry;
    try {
        entry = create();
    } catch (const std::exception& error) {
        entry = PipelineEntry{};
        entry.error = std::string("pipeline creation threw: ") + error.what();
    }
    guard.lock();
    slot.entry = entry;
    slot.done = true;
    switch (entry.origin) {
    case PipelineOrigin::Ready:
        ++stats_.ready;
        break;
    case PipelineOrigin::Compiled:
        ++stats_.compiled;
        break;
    case PipelineOrigin::Refused:
        ++stats_.refused;
        break;
    case PipelineOrigin::Failed:
        ++stats_.failed;
        break;
    }
    finished_.notify_all();
    return entry;
}

PipelineLibraryStats PipelineLibrary::Stats() const {
    std::lock_guard<std::mutex> guard(lock_);
    return stats_;
}

PipelineEntry PipelineLibrary::CreateGraphics(const GraphicsRequest& request) const {
    PipelineEntry entry;
    const auto start = std::chrono::steady_clock::now();
    const auto finish = [&entry, start] {
        entry.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return entry;
    };
    const DeviceFunctions& vk = device_.Vk();
    std::vector<GraphicsStage> stages;
    const auto release = [&vk, this, &stages] {
        for (const GraphicsStage& stage : stages) {
            vk.destroyShaderModule(device_.Handle(), stage.module, nullptr);
        }
    };
    for (const GraphicsShaderSource& shader : request.shaders) {
        const std::optional<std::vector<std::uint32_t>> words = ReadTitleModule(shader.module, entry.usesCodeBase, entry.error);
        if (!words) {
            release();
            return finish();
        }
        entry.bytes += words->size() * sizeof(std::uint32_t);
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = words->size() * sizeof(std::uint32_t);
        moduleInfo.pCode = words->data();
        VkShaderModule module = VK_NULL_HANDLE;
        const VkResult moduleResult = vk.createShaderModule(device_.Handle(), &moduleInfo, nullptr, &module);
        if (moduleResult != VK_SUCCESS) {
            release();
            entry.error = "vkCreateShaderModule failed with " + std::to_string(static_cast<int>(moduleResult));
            return finish();
        }
        stages.push_back({shader.stage, module, shader.subgroupSize});
    }
    const bool overLimit = compileLimitBytes_ != 0 && entry.bytes > compileLimitBytes_;
    const std::string limitText = std::to_string(entry.bytes) + " bytes of modules, limit " + std::to_string(compileLimitBytes_);
    if (overLimit && !device_.PipelineCacheControl()) {
        release();
        entry.origin = PipelineOrigin::Refused;
        entry.error = "the modules are larger than the run-time compile limit (" + limitText + ") and this device cannot tell whether the pipeline cache holds the pipeline";
        return finish();
    }
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = VK_PIPELINE_COMPILE_REQUIRED;
    if (device_.PipelineCacheControl()) {
        result = CreateGraphicsPipeline(device_, cache_, request.state, stages, request.input, VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT, &pipeline, entry.error);
        if (result == VK_SUCCESS) {
            entry.origin = PipelineOrigin::Ready;
        }
    }
    if (result == VK_PIPELINE_COMPILE_REQUIRED) {
        if (overLimit) {
            entry.origin = PipelineOrigin::Refused;
            entry.error = "the pipeline is not in the pipeline cache and its modules are larger than the run-time compile limit (" + limitText +
                          "); build the cache offline with the shader_pipelines target";
        } else {
            result = CreateGraphicsPipeline(device_, cache_, request.state, stages, request.input, 0, &pipeline, entry.error);
            if (result == VK_SUCCESS) {
                entry.origin = PipelineOrigin::Compiled;
            }
        }
    }
    release();
    if (entry.origin == PipelineOrigin::Refused) {
        return finish();
    }
    if (result != VK_SUCCESS) {
        entry.origin = PipelineOrigin::Failed;
        return finish();
    }
    entry.pipeline = pipeline;
    return finish();
}

PipelineEntry PipelineLibrary::Create(const PipelineSource& source) const {
    PipelineEntry entry;
    const auto start = std::chrono::steady_clock::now();
    const auto finish = [&entry, start] {
        entry.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return entry;
    };
    const std::optional<std::vector<std::uint32_t>> words = ReadTitleModule(source.module, entry.usesCodeBase, entry.error);
    if (!words) {
        return finish();
    }
    entry.bytes = words->size() * sizeof(std::uint32_t);
    const bool overLimit = compileLimitBytes_ != 0 && entry.bytes > compileLimitBytes_;
    const std::string limitText = std::to_string(entry.bytes) + " bytes, limit " + std::to_string(compileLimitBytes_);
    if (overLimit && !device_.PipelineCacheControl()) {
        entry.origin = PipelineOrigin::Refused;
        entry.error = "the module is larger than the run-time compile limit (" + limitText + ") and this device cannot tell whether the pipeline cache holds it";
        return finish();
    }
    const DeviceFunctions& vk = device_.Vk();
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = entry.bytes;
    moduleInfo.pCode = words->data();
    VkShaderModule module = VK_NULL_HANDLE;
    const VkResult moduleResult = vk.createShaderModule(device_.Handle(), &moduleInfo, nullptr, &module);
    if (moduleResult != VK_SUCCESS) {
        entry.error = "vkCreateShaderModule failed with " + std::to_string(static_cast<int>(moduleResult));
        return finish();
    }
    VkPipelineCreationFeedback feedback{};
    VkPipelineCreationFeedback stageFeedback{};
    VkPipelineCreationFeedbackCreateInfo feedbackInfo{VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO};
    feedbackInfo.pPipelineCreationFeedback = &feedback;
    feedbackInfo.pipelineStageCreationFeedbackCount = 1;
    feedbackInfo.pPipelineStageCreationFeedbacks = &stageFeedback;
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    subgroup.requiredSubgroupSize = source.subgroupSize;
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, &feedbackInfo};
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = device_.TitlePipelineLayout();
    if (source.subgroupSize != 0 && device_.CanRequireSubgroupSize(source.subgroupSize, VK_SHADER_STAGE_COMPUTE_BIT)) {
        info.stage.pNext = &subgroup;
        entry.subgroupForced = true;
    }
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = VK_PIPELINE_COMPILE_REQUIRED;
    if (device_.PipelineCacheControl()) {
        info.flags = VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
        result = vk.createComputePipelines(device_.Handle(), cache_, 1, &info, nullptr, &pipeline);
        if (result == VK_SUCCESS) {
            entry.origin = PipelineOrigin::Ready;
        }
    }
    if (result == VK_PIPELINE_COMPILE_REQUIRED) {
        pipeline = VK_NULL_HANDLE;
        if (overLimit) {
            entry.origin = PipelineOrigin::Refused;
            entry.error = "the pipeline is not in the pipeline cache and the module is larger than the run-time compile limit (" + limitText +
                          "); build the cache offline with the shader_pipelines target";
        } else {
            info.flags = 0;
            feedback = {};
            stageFeedback = {};
            result = vk.createComputePipelines(device_.Handle(), cache_, 1, &info, nullptr, &pipeline);
            if (result == VK_SUCCESS) {
                entry.origin = PipelineOrigin::Compiled;
            }
        }
    }
    vk.destroyShaderModule(device_.Handle(), module, nullptr);
    if (entry.origin == PipelineOrigin::Refused) {
        return finish();
    }
    if (result != VK_SUCCESS) {
        entry.origin = PipelineOrigin::Failed;
        entry.error = "vkCreateComputePipelines failed with " + std::to_string(static_cast<int>(result));
        return finish();
    }
    entry.pipeline = pipeline;
    entry.driverCacheHit = (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) != 0 &&
                           (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0;
    if (!device_.PipelineCacheControl()) {
        entry.origin = entry.driverCacheHit ? PipelineOrigin::Ready : PipelineOrigin::Compiled;
    }
    return finish();
}

}
