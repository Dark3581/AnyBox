#include "ResourceState.hpp"

#include <cstdio>

namespace abx::d3d12x {

namespace {

constexpr UINT kWriteStates = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_COPY_DEST |
                              D3D12_RESOURCE_STATE_RESOLVE_DEST | D3D12_RESOURCE_STATE_STREAM_OUT;
constexpr UINT kUnsupportedStates = D3D12_RESOURCE_STATE_STREAM_OUT | D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE | D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE |
                                    D3D12_RESOURCE_STATE_VIDEO_DECODE_READ | D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE | D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ |
                                    D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE | D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ | D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE;
constexpr UINT kKnownStates = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER | kWriteStates | D3D12_RESOURCE_STATE_DEPTH_READ |
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT |
                              D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_RESOLVE_SOURCE | kUnsupportedStates;

std::string Hex(UINT value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%x", value);
    return text;
}

struct StateUsage {
    UINT state;
    VkPipelineStageFlags2 stages;
    VkAccessFlags2 access;
    VkImageLayout layout;
};

const StateUsage kUsages[] = {
    {D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | kAllShaderStages,
     VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED},
    {D3D12_RESOURCE_STATE_INDEX_BUFFER, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED},
    {D3D12_RESOURCE_STATE_RENDER_TARGET, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
    {D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kAllShaderStages, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL},
    {D3D12_RESOURCE_STATE_DEPTH_WRITE, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL},
    {D3D12_RESOURCE_STATE_DEPTH_READ, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
     VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
    {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    {D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    {D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED},
    {D3D12_RESOURCE_STATE_COPY_DEST, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL},
    {D3D12_RESOURCE_STATE_COPY_SOURCE, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
    {D3D12_RESOURCE_STATE_RESOLVE_DEST, VK_PIPELINE_STAGE_2_RESOLVE_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL},
    {D3D12_RESOURCE_STATE_RESOLVE_SOURCE, VK_PIPELINE_STAGE_2_RESOLVE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
};

}

std::optional<ResourceUsage> UsageOfState(D3D12_RESOURCE_STATES states, bool depthStencil, std::string& error) {
    const UINT bits = static_cast<UINT>(states);
    if ((bits & ~kKnownStates) != 0) {
        error = "resource state bits " + Hex(bits & ~kKnownStates) + " are unknown";
        return std::nullopt;
    }
    if ((bits & kUnsupportedStates) != 0) {
        error = "resource state bits " + Hex(bits & kUnsupportedStates) + " (stream output, ray tracing, shading rate or video) are not supported";
        return std::nullopt;
    }
    ResourceUsage usage;
    if (bits == 0) {
        usage.stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        usage.access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        usage.layout = VK_IMAGE_LAYOUT_GENERAL;
        return usage;
    }
    const UINT writes = bits & kWriteStates;
    if (writes != 0 && writes != bits) {
        error = "resource state " + Hex(bits) + " combines a write state with other states";
        return std::nullopt;
    }
    if ((writes & (writes - 1)) != 0) {
        error = "resource state " + Hex(bits) + " combines write states";
        return std::nullopt;
    }
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool conflicting = false;
    for (const StateUsage& entry : kUsages) {
        if ((bits & entry.state) == 0) {
            continue;
        }
        usage.stages |= entry.stages;
        usage.access |= entry.access;
        VkImageLayout wanted = entry.layout;
        if (wanted == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && depthStencil) {
            wanted = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        }
        if (wanted == VK_IMAGE_LAYOUT_UNDEFINED) {
            continue;
        }
        if (layout != VK_IMAGE_LAYOUT_UNDEFINED && layout != wanted) {
            conflicting = true;
        }
        layout = wanted;
    }
    usage.layout = conflicting || layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_GENERAL : layout;
    return usage;
}

std::optional<VkImageMemoryBarrier2> ImageTransition(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, bool depthStencil, std::string& error) {
    const std::optional<ResourceUsage> from = UsageOfState(before, depthStencil, error);
    const std::optional<ResourceUsage> to = from ? UsageOfState(after, depthStencil, error) : std::nullopt;
    if (!from || !to) {
        return std::nullopt;
    }
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = from->stages;
    barrier.srcAccessMask = from->access;
    barrier.dstStageMask = to->stages;
    barrier.dstAccessMask = to->access;
    barrier.oldLayout = from->layout;
    barrier.newLayout = to->layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    return barrier;
}

std::optional<VkMemoryBarrier2> BufferTransition(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, std::string& error) {
    const std::optional<ResourceUsage> from = UsageOfState(before, false, error);
    const std::optional<ResourceUsage> to = from ? UsageOfState(after, false, error) : std::nullopt;
    if (!from || !to) {
        return std::nullopt;
    }
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = from->stages;
    barrier.srcAccessMask = from->access;
    barrier.dstStageMask = to->stages;
    barrier.dstAccessMask = to->access;
    return barrier;
}

VkMemoryBarrier2 UnorderedAccessBarrier() {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = kAllShaderStages;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = kAllShaderStages;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    return barrier;
}

VkMemoryBarrier2 AliasingBarrier() {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    return barrier;
}

}
