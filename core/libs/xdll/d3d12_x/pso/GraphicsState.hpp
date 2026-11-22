#pragma once

#include "GraphicsPipeline.hpp"

#include <windows.h>
#include <d3d12.h>

#include <optional>
#include <string>

namespace abx::d3d12x {

std::optional<VkFormat> VulkanFormat(DXGI_FORMAT format);
bool FormatHasStencil(DXGI_FORMAT format);
std::optional<VkBlendFactor> VulkanBlendFactor(D3D12_BLEND blend);
std::optional<VkBlendOp> VulkanBlendOp(D3D12_BLEND_OP op);
std::optional<VkLogicOp> VulkanLogicOp(D3D12_LOGIC_OP op);
std::optional<VkCompareOp> VulkanCompareOp(D3D12_COMPARISON_FUNC function);
std::optional<VkStencilOp> VulkanStencilOp(D3D12_STENCIL_OP op);
std::optional<VkPrimitiveTopology> VulkanTopologyClass(D3D12_PRIMITIVE_TOPOLOGY_TYPE type);
std::optional<VkSampleCountFlagBits> VulkanSampleCount(UINT count);

std::optional<GraphicsPipelineState> TranslateGraphicsState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, std::string& error);

}
