#include "CopyLayout.hpp"

#include <algorithm>

namespace abx::d3d12x {

namespace {

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

std::uint32_t DivideUp(std::uint32_t value, std::uint32_t divisor) {
    return (value + divisor - 1) / divisor;
}

}

std::optional<CopyBlock> CopyBlockOf(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return CopyBlock{1, 1, 16};
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
        return CopyBlock{1, 1, 12};
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
        return CopyBlock{1, 1, 8};
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return CopyBlock{1, 1, 4};
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM:
        return CopyBlock{1, 1, 2};
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_A8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
        return CopyBlock{1, 1, 1};
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        return CopyBlock{4, 4, 8};
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return CopyBlock{4, 4, 16};
    default:
        return std::nullopt;
    }
}

std::uint32_t MipExtent(std::uint64_t size, std::uint32_t mip) {
    const std::uint64_t value = mip >= 64 ? 0 : size >> mip;
    return value == 0 ? 1u : static_cast<std::uint32_t>(value);
}

std::optional<Subresource> SplitSubresource(const D3D12_RESOURCE_DESC& desc, UINT index, std::uint32_t mipLevels) {
    const std::uint32_t layers = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : desc.DepthOrArraySize;
    if (mipLevels == 0 || layers == 0) {
        return std::nullopt;
    }
    const std::uint64_t perPlane = static_cast<std::uint64_t>(mipLevels) * layers;
    const std::uint32_t plane = static_cast<std::uint32_t>(index / perPlane);
    if (plane > 1) {
        return std::nullopt;
    }
    return Subresource{index % mipLevels, static_cast<std::uint32_t>((index / mipLevels) % layers), plane};
}

std::optional<VkImageAspectFlags> PlaneAspect(VkImageAspectFlags aspects, std::uint32_t plane) {
    const bool depth = (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    const bool stencil = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    if (plane == 0) {
        return depth ? VK_IMAGE_ASPECT_DEPTH_BIT : (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (plane == 1 && depth && stencil) {
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    return std::nullopt;
}

std::optional<Footprints> DesktopCopyableFootprints(const D3D12_RESOURCE_DESC& desc, std::uint32_t mipLevels, UINT first, UINT count, UINT64 baseOffset, std::string& error) {
    const std::optional<CopyBlock> block = CopyBlockOf(desc.Format);
    if (!block) {
        error = "format " + std::to_string(static_cast<int>(desc.Format)) + " has no copy layout";
        return std::nullopt;
    }
    Footprints result;
    UINT64 offset = baseOffset;
    for (UINT index = first; index < first + count; ++index) {
        const std::optional<Subresource> sub = SplitSubresource(desc, index, mipLevels);
        if (!sub || sub->plane != 0) {
            error = "subresource " + std::to_string(index) + " is outside the texture";
            return std::nullopt;
        }
        const std::uint32_t width = MipExtent(desc.Width, sub->mip);
        const std::uint32_t height = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1u : MipExtent(desc.Height, sub->mip);
        const std::uint32_t depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? MipExtent(desc.DepthOrArraySize, sub->mip) : 1u;
        const std::uint32_t blocksWide = DivideUp(width, block->width);
        const std::uint32_t blocksHigh = DivideUp(height, block->height);
        const UINT64 rowBytes = static_cast<UINT64>(blocksWide) * block->bytes;
        const UINT64 pitch = AlignUp(rowBytes, kTexturePitchAlignment);
        offset = AlignUp(offset, kTexturePlacementAlignment);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        layout.Offset = offset;
        layout.Footprint.Format = desc.Format;
        layout.Footprint.Width = blocksWide * block->width;
        layout.Footprint.Height = blocksHigh * block->height;
        layout.Footprint.Depth = depth;
        layout.Footprint.RowPitch = static_cast<UINT>(pitch);
        result.layouts.push_back(layout);
        result.rows.push_back(blocksHigh);
        result.rowBytes.push_back(rowBytes);
        const UINT64 size = pitch * (static_cast<UINT64>(blocksHigh) * depth - 1) + rowBytes;
        result.totalBytes = offset + size - baseOffset;
        offset += size;
    }
    return result;
}

std::optional<VkBufferImageCopy> FootprintRegion(const D3D12_RESOURCE_DESC& texture, std::uint32_t mipLevels, VkImageAspectFlags aspects, UINT subresource,
                                                 const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint, UINT x, UINT y, UINT z, const D3D12_BOX* box, std::string& error) {
    const std::optional<Subresource> sub = SplitSubresource(texture, subresource, mipLevels);
    if (!sub) {
        error = "subresource " + std::to_string(subresource) + " is outside the texture";
        return std::nullopt;
    }
    const std::optional<VkImageAspectFlags> planeAspect = PlaneAspect(aspects, sub->plane);
    if (!planeAspect) {
        error = "subresource " + std::to_string(subresource) + " names a plane the texture does not have";
        return std::nullopt;
    }
    const VkImageAspectFlags aspect = *planeAspect;
    const std::optional<CopyBlock> block = CopyBlockOf(footprint.Footprint.Format);
    if (!block) {
        error = "footprint format " + std::to_string(static_cast<int>(footprint.Footprint.Format)) + " has no copy layout";
        return std::nullopt;
    }
    if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT && block->bytes != 1) {
        error = "a stencil plane copy needs a footprint format of 1 byte per texel";
        return std::nullopt;
    }
    if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT && (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0 && block->bytes != 4) {
        error = "the depth plane of a depth-stencil texture copies as 4 bytes per texel";
        return std::nullopt;
    }
    if (footprint.Footprint.RowPitch % block->bytes != 0 || footprint.Offset % block->bytes != 0 || footprint.Footprint.Width == 0 || footprint.Footprint.Height == 0 ||
        footprint.Footprint.Depth == 0) {
        error = "the footprint's row pitch or offset is not a whole number of blocks, or its extent is zero";
        return std::nullopt;
    }
    const std::uint64_t rowBytes = static_cast<std::uint64_t>(DivideUp(footprint.Footprint.Width, block->width)) * block->bytes;
    if (footprint.Footprint.RowPitch < rowBytes) {
        error = "the footprint's row pitch is shorter than a row";
        return std::nullopt;
    }
    const std::uint32_t mipWidth = MipExtent(texture.Width, sub->mip);
    const std::uint32_t mipHeight = texture.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1u : MipExtent(texture.Height, sub->mip);
    const std::uint32_t mipDepth = texture.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? MipExtent(texture.DepthOrArraySize, sub->mip) : 1u;
    UINT left = 0;
    UINT top = 0;
    UINT front = 0;
    UINT right = footprint.Footprint.Width;
    UINT bottom = footprint.Footprint.Height;
    UINT back = footprint.Footprint.Depth;
    if (box != nullptr) {
        if (box->left >= box->right || box->top >= box->bottom || box->front >= box->back || box->right > footprint.Footprint.Width ||
            box->bottom > footprint.Footprint.Height || box->back > footprint.Footprint.Depth) {
            error = "the copy box is empty or outside the footprint";
            return std::nullopt;
        }
        left = box->left;
        top = box->top;
        front = box->front;
        right = box->right;
        bottom = box->bottom;
        back = box->back;
    }
    if (left % block->width != 0 || top % block->height != 0 || x % block->width != 0 || y % block->height != 0) {
        error = "a block-compressed copy does not start on a block boundary";
        return std::nullopt;
    }
    if (x >= mipWidth || y >= mipHeight || z >= mipDepth) {
        error = "the copy starts outside mip " + std::to_string(sub->mip);
        return std::nullopt;
    }
    VkBufferImageCopy region{};
    region.bufferOffset = footprint.Offset + static_cast<VkDeviceSize>(front) * footprint.Footprint.RowPitch * DivideUp(footprint.Footprint.Height, block->height) +
                          static_cast<VkDeviceSize>(top / block->height) * footprint.Footprint.RowPitch + static_cast<VkDeviceSize>(left / block->width) * block->bytes;
    region.bufferRowLength = footprint.Footprint.RowPitch / block->bytes * block->width;
    region.bufferImageHeight = DivideUp(footprint.Footprint.Height, block->height) * block->height;
    region.imageSubresource = {aspect, sub->mip, sub->layer, 1};
    region.imageOffset = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), static_cast<std::int32_t>(z)};
    const auto fit = [&error](std::uint32_t wanted, std::uint32_t available, std::uint32_t blockSize, std::uint32_t& out) {
        if (wanted <= available) {
            out = wanted;
            return true;
        }
        if (blockSize > 1 && wanted <= DivideUp(available, blockSize) * blockSize) {
            out = available;
            return true;
        }
        error = "the copy runs past the edge of the mip";
        return false;
    };
    if (!fit(right - left, mipWidth - x, block->width, region.imageExtent.width) || !fit(bottom - top, mipHeight - y, block->height, region.imageExtent.height) ||
        !fit(back - front, mipDepth - z, 1, region.imageExtent.depth)) {
        return std::nullopt;
    }
    if (texture.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        region.imageSubresource.baseArrayLayer = 0;
    }
    return region;
}

}
