#include "IndirectClamp.hpp"

#include "AbxApi.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace abx::d3d12x {

namespace {

const std::uint32_t kClampSpirv[] = {
#include "IndirectClampSpirv.inc"
};

struct ClampPush {
    std::uint64_t arguments;
    std::uint64_t stats;
    std::uint32_t records;
    std::uint32_t strideWords;
    std::uint32_t maxX;
    std::uint32_t maxY;
    std::uint32_t maxZ;
    std::uint32_t maxGroups;
};

void Barrier(const VulkanDevice& device, VkCommandBuffer commands, VkPipelineStageFlags2 fromStage, VkAccessFlags2 fromAccess, VkPipelineStageFlags2 toStage, VkAccessFlags2 toAccess) {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = fromStage;
    barrier.srcAccessMask = fromAccess;
    barrier.dstStageMask = toStage;
    barrier.dstAccessMask = toAccess;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    device.Vk().cmdPipelineBarrier2(commands, &dependency);
}

}

IndirectClamp* IndirectClamp::For(const VulkanDevice& device) {
    static std::mutex lock;
    static IndirectClamp* created = nullptr;
    static bool tried = false;
    const std::lock_guard guard(lock);
    if (tried) return created;
    tried = true;
    const char* setting = AbxSetting("d3d12_x.clamp_indirect_dispatches");
    if (setting != nullptr && setting[0] == '0') {
        AbxLog("d3d12_x", "indirect dispatch arguments are not checked before the GPU reads them (d3d12_x.clamp_indirect_dispatches = 0)");
        return nullptr;
    }
    auto* clamp = new IndirectClamp(device);
    std::string error;
    if (!clamp->Create(error)) {
        AbxLog("d3d12_x", "indirect dispatch arguments cannot be checked on this device: %s", error.c_str());
        delete clamp;
        return nullptr;
    }
    AbxLog("d3d12_x", "indirect dispatch arguments are copied and checked on the GPU before they are used: a record asking for more than %u x %u x %u groups (the device's limits, "
                      "each capped at the total) or %u in all runs empty, as counts left by work AnyBox skipped would otherwise run until Windows resets the GPU "
                      "(d3d12_x.clamp_indirect_dispatches = 0 turns this off)",
           clamp->maxDimension_[0], clamp->maxDimension_[1], clamp->maxDimension_[2], kMaxGroups);
    created = clamp;
    return created;
}

bool IndirectClamp::Create(std::string& error) {
    const DeviceFunctions& vk = device_.Vk();
    if (!device_.BufferDeviceAddress()) {
        error = "the device has no buffer device addresses";
        return false;
    }
    const std::uint32_t* limits = device_.Properties().limits.maxComputeWorkGroupCount;
    for (int axis = 0; axis < 3; ++axis) maxDimension_[axis] = (std::min)(limits[axis], kMaxGroups);
    meshMaxGroups_ = (std::min)(device_.MeshMaxTotal(), kMaxGroups);
    for (int axis = 0; axis < 3; ++axis) meshMaxDimension_[axis] = (std::min)(device_.MeshMaxGroups()[axis], meshMaxGroups_);
    if (!CreateHostBuffer(device_, kScratchBytes,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, scratch_,
                          error)) {
        return false;
    }
    if (scratch_.address == 0 || scratch_.mapped == nullptr) {
        error = "the scratch buffer has no device address or is not mapped";
        return false;
    }
    std::memset(scratch_.mapped, 0, static_cast<std::size_t>(kHeaderBytes));
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = sizeof(kClampSpirv);
    moduleInfo.pCode = kClampSpirv;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vk.createShaderModule(device_.Handle(), &moduleInfo, nullptr, &module) != VK_SUCCESS) {
        error = "vkCreateShaderModule failed for the argument check";
        return false;
    }
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ClampPush)};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vk.createPipelineLayout(device_.Handle(), &layoutInfo, nullptr, &layout_) != VK_SUCCESS) {
        vk.destroyShaderModule(device_.Handle(), module, nullptr);
        error = "vkCreatePipelineLayout failed for the argument check";
        return false;
    }
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = layout_;
    const VkResult result = vk.createComputePipelines(device_.Handle(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
    vk.destroyShaderModule(device_.Handle(), module, nullptr);
    if (result != VK_SUCCESS) {
        pipeline_ = VK_NULL_HANDLE;
        error = "vkCreateComputePipelines failed for the argument check with " + std::to_string(static_cast<int>(result));
        return false;
    }
    return true;
}

void IndirectClamp::Report() {
    std::uint32_t stats[4];
    std::memcpy(stats, scratch_.mapped, sizeof(stats));
    const std::uint32_t before = reported_.load();
    if (stats[0] == before || (stats[0] > 8 && stats[0] / 2 < before)) return;
    reported_.store(stats[0]);
    AbxLog("d3d12_x", "the GPU found %u indirect dispatch record(s) with impossible group counts so far and ran them empty (last: %u x %u x %u): work that should have written them was "
                      "skipped or wrong",
           stats[0], stats[1], stats[2], stats[3]);
}

std::optional<ClampedArguments> IndirectClamp::RecordDispatches(VkCommandBuffer commands, VkBuffer arguments, VkDeviceSize offset, std::uint32_t records, std::uint32_t stride,
                                                              bool mesh, std::string& error) {
    if (records == 0 || stride < 12 || stride % 4 != 0) {
        error = "indirect dispatch records with a stride that is not a multiple of 4 or below 12 bytes";
        return std::nullopt;
    }
    const VkDeviceSize span = static_cast<VkDeviceSize>(stride) * (records - 1) + 12;
    const VkDeviceSize room = (span + 15) & ~VkDeviceSize{15};
    const VkDeviceSize usable = kScratchBytes - kHeaderBytes;
    if (room > usable / 4) {
        error = "indirect dispatch records of " + std::to_string(span) + " bytes are more than the argument check keeps";
        return std::nullopt;
    }
    Report();
    VkDeviceSize at = 0;
    for (;;) {
        std::uint64_t current = next_.load();
        const std::uint64_t start = (current % usable) + room > usable ? current + (usable - current % usable) : current;
        if (next_.compare_exchange_weak(current, start + room)) {
            at = kHeaderBytes + start % usable;
            break;
        }
    }
    const DeviceFunctions& vk = device_.Vk();
    Barrier(device_, commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkBufferCopy region{offset, at, span};
    vk.cmdCopyBuffer(commands, arguments, scratch_.buffer, 1, &region);
    Barrier(device_, commands, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    ClampPush push{};
    push.arguments = scratch_.address + at;
    push.stats = scratch_.address;
    push.records = records;
    push.strideWords = stride / 4;
    const std::uint32_t* dimensions = mesh ? meshMaxDimension_ : maxDimension_;
    push.maxX = dimensions[0];
    push.maxY = dimensions[1];
    push.maxZ = dimensions[2];
    push.maxGroups = mesh ? meshMaxGroups_ : kMaxGroups;
    vk.cmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vk.cmdPushConstants(commands, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vk.cmdDispatch(commands, 1, 1, 1);
    Barrier(device_, commands, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    return ClampedArguments{scratch_.buffer, at, span};
}

}
