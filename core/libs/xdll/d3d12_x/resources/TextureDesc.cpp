#include "TextureDesc.hpp"
#include "ImageDescriptor.hpp"

#include "GraphicsState.hpp"

namespace abx::d3d12x {

namespace {

constexpr UINT kKnownFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                             D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE | D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

TextureFormat Color(VkFormat format, bool mutableFormat = false, bool compressed = false) {
    return {format, mutableFormat, VK_IMAGE_ASPECT_COLOR_BIT, compressed};
}

TextureFormat Depth(VkFormat format, bool stencil) {
    return {format, false, static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)), false};
}

std::optional<TextureFormat> TypelessOrBlock(DXGI_FORMAT format, bool depthStencil) {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return Color(VK_FORMAT_R32G32B32A32_UINT, true);
    case DXGI_FORMAT_R32G32B32_TYPELESS: return Color(VK_FORMAT_R32G32B32_UINT, true);
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return Color(VK_FORMAT_R16G16B16A16_UINT, true);
    case DXGI_FORMAT_R32G32_TYPELESS: return Color(VK_FORMAT_R32G32_UINT, true);
    case DXGI_FORMAT_R32G8X24_TYPELESS: return Depth(VK_FORMAT_D32_SFLOAT_S8_UINT, true);
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return Color(VK_FORMAT_A2B10G10R10_UNORM_PACK32, true);
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return Color(VK_FORMAT_R8G8B8A8_UNORM, true);
    case DXGI_FORMAT_R16G16_TYPELESS: return Color(VK_FORMAT_R16G16_UINT, true);
    case DXGI_FORMAT_R32_TYPELESS: return depthStencil ? Depth(VK_FORMAT_D32_SFLOAT, false) : Color(VK_FORMAT_R32_UINT, true);
    case DXGI_FORMAT_R24G8_TYPELESS: return Depth(VK_FORMAT_D24_UNORM_S8_UINT, true);
    case DXGI_FORMAT_R8G8_TYPELESS: return Color(VK_FORMAT_R8G8_UNORM, true);
    case DXGI_FORMAT_R16_TYPELESS: return depthStencil ? Depth(VK_FORMAT_D16_UNORM, false) : Color(VK_FORMAT_R16_UNORM, true);
    case DXGI_FORMAT_R8_TYPELESS: return Color(VK_FORMAT_R8_UNORM, true);
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return Color(VK_FORMAT_B8G8R8A8_UNORM, true);
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return Color(VK_FORMAT_B8G8R8A8_UNORM, true);
    case DXGI_FORMAT_B8G8R8X8_UNORM: return Color(VK_FORMAT_B8G8R8A8_UNORM);
    case DXGI_FORMAT_A8_UNORM: return Color(VK_FORMAT_A8_UNORM_KHR);
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return Color(VK_FORMAT_B8G8R8A8_SRGB);
    case DXGI_FORMAT_BC1_TYPELESS: return Color(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, true, true);
    case DXGI_FORMAT_BC1_UNORM: return Color(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, false, true);
    case DXGI_FORMAT_BC1_UNORM_SRGB: return Color(VK_FORMAT_BC1_RGBA_SRGB_BLOCK, false, true);
    case DXGI_FORMAT_BC2_TYPELESS: return Color(VK_FORMAT_BC2_UNORM_BLOCK, true, true);
    case DXGI_FORMAT_BC2_UNORM: return Color(VK_FORMAT_BC2_UNORM_BLOCK, false, true);
    case DXGI_FORMAT_BC2_UNORM_SRGB: return Color(VK_FORMAT_BC2_SRGB_BLOCK, false, true);
    case DXGI_FORMAT_BC3_TYPELESS: return Color(VK_FORMAT_BC3_UNORM_BLOCK, true, true);
    case DXGI_FORMAT_BC3_UNORM: return Color(VK_FORMAT_BC3_UNORM_BLOCK, false, true);
    case DXGI_FORMAT_BC3_UNORM_SRGB: return Color(VK_FORMAT_BC3_SRGB_BLOCK, false, true);
    case DXGI_FORMAT_BC4_TYPELESS: return Color(VK_FORMAT_BC4_UNORM_BLOCK, true, true);
    case DXGI_FORMAT_BC4_UNORM: return Color(VK_FORMAT_BC4_UNORM_BLOCK, false, true);
    case DXGI_FORMAT_BC4_SNORM: return Color(VK_FORMAT_BC4_SNORM_BLOCK, false, true);
    case DXGI_FORMAT_BC5_TYPELESS: return Color(VK_FORMAT_BC5_UNORM_BLOCK, true, true);
    case DXGI_FORMAT_BC5_UNORM: return Color(VK_FORMAT_BC5_UNORM_BLOCK, false, true);
    case DXGI_FORMAT_BC5_SNORM: return Color(VK_FORMAT_BC5_SNORM_BLOCK, false, true);
    case DXGI_FORMAT_BC6H_TYPELESS: return Color(VK_FORMAT_BC6H_UFLOAT_BLOCK, true, true);
    case DXGI_FORMAT_BC6H_UF16: return Color(VK_FORMAT_BC6H_UFLOAT_BLOCK, false, true);
    case DXGI_FORMAT_BC6H_SF16: return Color(VK_FORMAT_BC6H_SFLOAT_BLOCK, false, true);
    case DXGI_FORMAT_BC7_TYPELESS: return Color(VK_FORMAT_BC7_UNORM_BLOCK, true, true);
    case DXGI_FORMAT_BC7_UNORM: return Color(VK_FORMAT_BC7_UNORM_BLOCK, false, true);
    case DXGI_FORMAT_BC7_UNORM_SRGB: return Color(VK_FORMAT_BC7_SRGB_BLOCK, false, true);
    default: return std::nullopt;
    }
}

}

std::optional<TextureFormat> TextureStorageFormat(DXGI_FORMAT format, bool depthStencil) {
    if (const std::optional<TextureFormat> special = TypelessOrBlock(format, depthStencil)) {
        return special;
    }
    const std::optional<VkFormat> direct = VulkanFormat(format);
    if (!direct) {
        return std::nullopt;
    }
    switch (format) {
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_D32_FLOAT:
        return Depth(*direct, false);
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return Depth(*direct, true);
    default:
        return Color(*direct);
    }
}

std::uint32_t FullMipChain(std::uint64_t width, std::uint32_t height, std::uint32_t depth) {
    std::uint64_t largest = width;
    if (height > largest) largest = height;
    if (depth > largest) largest = depth;
    std::uint32_t levels = 1;
    while (largest > 1) {
        largest >>= 1;
        ++levels;
    }
    return levels;
}

std::optional<TextureImage> TranslateTextureDesc(const D3D12_RESOURCE_DESC& desc, std::string& error) {
    TextureImage image;
    VkImageCreateInfo& info = image.info;
    const UINT flags = static_cast<UINT>(desc.Flags);
    if ((flags & ~kKnownFlags) != 0) {
        error = "resource flags " + std::to_string(flags & ~kKnownFlags) + " are not supported";
        return std::nullopt;
    }
    const bool renderTarget = (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
    const bool depthStencil = (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    const bool unorderedAccess = (flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
    const bool denyShaderResource = (flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0;
    if (desc.Width == 0 || desc.Width > 0xFFFFFFFFull || desc.Height == 0 || desc.DepthOrArraySize == 0) {
        error = "the texture has a zero or oversized extent";
        return std::nullopt;
    }
    switch (desc.Dimension) {
    case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
        if (desc.Height != 1) {
            error = "a 1D texture has height " + std::to_string(desc.Height);
            return std::nullopt;
        }
        info.imageType = VK_IMAGE_TYPE_1D;
        info.extent = {static_cast<std::uint32_t>(desc.Width), 1, 1};
        info.arrayLayers = desc.DepthOrArraySize;
        break;
    case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
        info.imageType = VK_IMAGE_TYPE_2D;
        info.extent = {static_cast<std::uint32_t>(desc.Width), desc.Height, 1};
        info.arrayLayers = desc.DepthOrArraySize;
        break;
    case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
        info.imageType = VK_IMAGE_TYPE_3D;
        info.extent = {static_cast<std::uint32_t>(desc.Width), desc.Height, desc.DepthOrArraySize};
        info.arrayLayers = 1;
        break;
    default:
        error = "resource dimension " + std::to_string(static_cast<int>(desc.Dimension)) + " is not a texture";
        return std::nullopt;
    }
    if (desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN) {
        error = "texture layout " + std::to_string(static_cast<int>(desc.Layout)) + " is not supported; only the driver-chosen layout is";
        return std::nullopt;
    }
    const std::optional<TextureFormat> format = TextureStorageFormat(desc.Format, depthStencil);
    if (!format) {
        error = "texture format " + std::to_string(static_cast<int>(desc.Format)) + " has no Vulkan mapping";
        return std::nullopt;
    }
    const bool isDepth = (format->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    if (depthStencil != isDepth) {
        error = depthStencil ? "a depth-stencil texture has the color format " + std::to_string(static_cast<int>(desc.Format))
                             : "depth format " + std::to_string(static_cast<int>(desc.Format)) + " without ALLOW_DEPTH_STENCIL";
        return std::nullopt;
    }
    if (isDepth && (renderTarget || unorderedAccess)) {
        error = "a depth-stencil texture cannot also allow render target or unordered access";
        return std::nullopt;
    }
    if (denyShaderResource && !depthStencil) {
        error = "DENY_SHADER_RESOURCE without ALLOW_DEPTH_STENCIL";
        return std::nullopt;
    }
    if (format->compressed && (renderTarget || depthStencil || unorderedAccess || desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)) {
        error = "a block-compressed texture is 1D or allows render target, depth-stencil or unordered access";
        return std::nullopt;
    }
    const std::uint32_t fullChain = FullMipChain(desc.Width, desc.Height, desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? desc.DepthOrArraySize : 1);
    info.mipLevels = desc.MipLevels == 0 ? fullChain : desc.MipLevels;
    if (info.mipLevels > fullChain) {
        error = std::to_string(desc.MipLevels) + " mip levels are more than the " + std::to_string(fullChain) + " of a full chain";
        return std::nullopt;
    }
    const std::optional<VkSampleCountFlagBits> samples = VulkanSampleCount(desc.SampleDesc.Count);
    if (!samples) {
        error = "sample count " + std::to_string(desc.SampleDesc.Count) + " is not supported";
        return std::nullopt;
    }
    if (*samples != VK_SAMPLE_COUNT_1_BIT && (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || info.mipLevels != 1 || unorderedAccess)) {
        error = "a multisampled texture must be 2D with one mip level and no unordered access";
        return std::nullopt;
    }
    info.samples = *samples;
    info.format = format->format;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (info.format == VK_FORMAT_A8_UNORM_KHR) image.features.insert("maintenance5");
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!denyShaderResource) info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (renderTarget) info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (depthStencil) info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (unorderedAccess) info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    const std::optional<ImageFormatInfo> component = ImageFormatOf(info.format);
    if (!denyShaderResource && component && component->component != ImageComponent::Float) info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (format->mutableFormat) info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width == desc.Height && desc.DepthOrArraySize % 6 == 0 && *samples == VK_SAMPLE_COUNT_1_BIT) {
        info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && renderTarget) {
        info.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    }
    if (format->compressed) {
        image.features.insert("textureCompressionBC");
    }
    image.format = *format;
    return image;
}

}
