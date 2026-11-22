#pragma once

#include "VulkanDevice.hpp"

#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace abx::d3d12x {

struct CopyBlock {
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::uint32_t bytes = 0;
};

struct Subresource {
    std::uint32_t mip = 0;
    std::uint32_t layer = 0;
    std::uint32_t plane = 0;
};

constexpr std::uint64_t kTexturePitchAlignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
constexpr std::uint64_t kTexturePlacementAlignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;

std::optional<CopyBlock> CopyBlockOf(DXGI_FORMAT format);
std::optional<Subresource> SplitSubresource(const D3D12_RESOURCE_DESC& desc, UINT index, std::uint32_t mipLevels);
std::uint32_t MipExtent(std::uint64_t size, std::uint32_t mip);

struct Footprints {
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts;
    std::vector<UINT> rows;
    std::vector<UINT64> rowBytes;
    UINT64 totalBytes = 0;
};

std::optional<Footprints> DesktopCopyableFootprints(const D3D12_RESOURCE_DESC& desc, std::uint32_t mipLevels, UINT first, UINT count, UINT64 baseOffset, std::string& error);

std::optional<VkBufferImageCopy> FootprintRegion(const D3D12_RESOURCE_DESC& texture, std::uint32_t mipLevels, VkImageAspectFlags aspects, UINT subresource,
                                                 const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint, UINT x, UINT y, UINT z, const D3D12_BOX* box, std::string& error);

}
