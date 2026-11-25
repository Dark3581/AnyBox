#pragma once

#include "VulkanDevice.hpp"

#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <optional>
#include <set>
#include <string>

namespace abx::d3d12x {

struct TextureFormat {
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool mutableFormat = false;
    VkImageAspectFlags aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    bool compressed = false;
};

std::optional<TextureFormat> TextureStorageFormat(DXGI_FORMAT format, bool depthStencil);
std::uint32_t FullMipChain(std::uint64_t width, std::uint32_t height, std::uint32_t depth);

struct TextureImage {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    TextureFormat format;
    std::set<std::string> features;
    bool depthRenderTarget = false;
    bool depthUnorderedAccess = false;
};

std::optional<TextureImage> TranslateTextureDesc(const D3D12_RESOURCE_DESC& desc, std::string& error);

}
