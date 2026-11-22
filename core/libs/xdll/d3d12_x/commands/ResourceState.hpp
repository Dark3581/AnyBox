#pragma once

#include "VulkanDevice.hpp"

#include <windows.h>
#include <d3d12.h>

#include <optional>
#include <string>

namespace abx::d3d12x {

struct ResourceUsage {
    VkPipelineStageFlags2 stages = 0;
    VkAccessFlags2 access = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
};

constexpr VkPipelineStageFlags2 kAllShaderStages = VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

std::optional<ResourceUsage> UsageOfState(D3D12_RESOURCE_STATES states, bool depthStencil, std::string& error);
std::optional<VkImageMemoryBarrier2> ImageTransition(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, bool depthStencil, std::string& error);
std::optional<VkMemoryBarrier2> BufferTransition(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, std::string& error);
VkMemoryBarrier2 UnorderedAccessBarrier();
VkMemoryBarrier2 AliasingBarrier();

}
