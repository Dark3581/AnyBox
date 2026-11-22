#pragma once

#include "TextureDesc.hpp"

#include <windows.h>
#include <d3d12.h>

#include <optional>
#include <set>
#include <string>

namespace abx::d3d12x {

struct ImageView {
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    std::set<std::string> features;
};

std::optional<VkComponentMapping> ComponentMapping(UINT mapping, bool alphaOne);
std::optional<ImageView> ShaderResourceView(const D3D12_RESOURCE_DESC& resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* view, std::string& error);
std::optional<ImageView> UnorderedAccessView(const D3D12_RESOURCE_DESC& resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* view, std::string& error);
std::optional<ImageView> RenderTargetView(const D3D12_RESOURCE_DESC& resource, const D3D12_RENDER_TARGET_VIEW_DESC* view, std::string& error);
std::optional<ImageView> DepthStencilView(const D3D12_RESOURCE_DESC& resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* view, std::string& error);

struct Sampler {
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSamplerReductionModeCreateInfo reduction{VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    bool usesReduction = false;
    std::set<std::string> features;
};

std::optional<Sampler> TranslateSampler(const D3D12_SAMPLER_DESC& desc, std::string& error);

}
