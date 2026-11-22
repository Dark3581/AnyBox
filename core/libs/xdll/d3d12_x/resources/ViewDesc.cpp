#include "ViewDesc.hpp"

#include "GraphicsState.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace abx::d3d12x {

namespace {

constexpr UINT kAll = (std::numeric_limits<UINT>::max)();

struct ViewFormat {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    bool alphaOne = false;
};

struct FormatFamily {
    UINT first;
    UINT last;
};

const FormatFamily kFamilies[] = {
    {1, 4}, {5, 8}, {9, 14}, {15, 18}, {19, 22}, {23, 25}, {27, 32}, {33, 38}, {39, 43}, {44, 47}, {48, 52}, {53, 59}, {60, 64},
    {70, 72}, {73, 75}, {76, 78}, {79, 81}, {82, 84}, {94, 96}, {97, 99},
};

UINT Family(DXGI_FORMAT format) {
    const UINT value = static_cast<UINT>(format);
    if (value == DXGI_FORMAT_B8G8R8A8_UNORM || value == DXGI_FORMAT_B8G8R8A8_TYPELESS || value == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    }
    if (value == DXGI_FORMAT_B8G8R8X8_UNORM || value == DXGI_FORMAT_B8G8R8X8_TYPELESS || value == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB) {
        return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    }
    for (const FormatFamily& family : kFamilies) {
        if (value >= family.first && value <= family.last) {
            return family.first;
        }
    }
    return value;
}

struct ViewTarget {
    TextureImage image;
    D3D12_RESOURCE_DESC resource{};
};

std::optional<ViewTarget> Target(const D3D12_RESOURCE_DESC& resource, std::string& error) {
    std::optional<TextureImage> image = TranslateTextureDesc(resource, error);
    if (!image) {
        return std::nullopt;
    }
    return ViewTarget{*image, resource};
}

std::optional<ViewFormat> ResolveFormat(const ViewTarget& target, DXGI_FORMAT requested, bool depthView, std::string& error) {
    const DXGI_FORMAT format = requested == DXGI_FORMAT_UNKNOWN ? target.resource.Format : requested;
    const TextureImage& image = target.image;
    const bool depthImage = (image.format.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    if (depthImage) {
        ViewFormat view{image.info.format, 0, false};
        switch (format) {
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            view.aspect = depthView ? image.format.aspects : static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT);
            break;
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
            view.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
            break;
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
            view.aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
            break;
        default:
            error = "view format " + std::to_string(static_cast<int>(format)) + " does not read a depth-stencil texture";
            return std::nullopt;
        }
        if ((view.aspect & image.format.aspects) != view.aspect) {
            error = "view format " + std::to_string(static_cast<int>(format)) + " reads a plane the texture does not have";
            return std::nullopt;
        }
        if (depthView && (format == DXGI_FORMAT_R16_UNORM || format == DXGI_FORMAT_R32_FLOAT || view.aspect == VK_IMAGE_ASPECT_STENCIL_BIT ||
                          format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS || format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS)) {
            error = "depth-stencil view format " + std::to_string(static_cast<int>(format)) + " is not a depth format";
            return std::nullopt;
        }
        return view;
    }
    if (depthView) {
        error = "a depth-stencil view of a texture without a depth format";
        return std::nullopt;
    }
    const std::optional<TextureFormat> resolved = TextureStorageFormat(format, false);
    if (!resolved || resolved->mutableFormat) {
        error = "view format " + std::to_string(static_cast<int>(format)) + (resolved ? " is typeless" : " has no Vulkan mapping");
        return std::nullopt;
    }
    if ((resolved->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) {
        error = "view format " + std::to_string(static_cast<int>(format)) + " is a depth format for a color texture";
        return std::nullopt;
    }
    if (format != target.resource.Format && Family(format) != Family(target.resource.Format)) {
        error = "view format " + std::to_string(static_cast<int>(format)) + " is not in the format family of the texture's " + std::to_string(static_cast<int>(target.resource.Format));
        return std::nullopt;
    }
    if (resolved->format != image.info.format && !image.format.mutableFormat) {
        error = "view format " + std::to_string(static_cast<int>(format)) + " differs from a texture that is not typeless";
        return std::nullopt;
    }
    return ViewFormat{resolved->format, VK_IMAGE_ASPECT_COLOR_BIT, format == DXGI_FORMAT_B8G8R8X8_UNORM || format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB};
}

bool Range(UINT first, UINT count, UINT total, std::uint32_t& outCount) {
    if (first >= total) {
        return false;
    }
    const UINT available = total - first;
    outCount = count == kAll ? available : count;
    return outCount != 0 && outCount <= available;
}

bool SetMips(VkImageViewCreateInfo& info, const TextureImage& image, UINT first, UINT count, std::string& error) {
    std::uint32_t levels = 0;
    if (!Range(first, count, image.info.mipLevels, levels)) {
        error = "mip range " + std::to_string(first) + "+" + std::to_string(count) + " is outside the texture's " + std::to_string(image.info.mipLevels);
        return false;
    }
    info.subresourceRange.baseMipLevel = first;
    info.subresourceRange.levelCount = levels;
    return true;
}

bool SetLayers(VkImageViewCreateInfo& info, std::uint32_t total, UINT first, UINT count, std::string& error) {
    std::uint32_t layers = 0;
    if (!Range(first, count, total, layers)) {
        error = "array range " + std::to_string(first) + "+" + std::to_string(count) + " is outside the texture's " + std::to_string(total);
        return false;
    }
    info.subresourceRange.baseArrayLayer = first;
    info.subresourceRange.layerCount = layers;
    return true;
}

bool ExpectDimension(const ViewTarget& target, D3D12_RESOURCE_DIMENSION dimension, bool multisampled, std::string& error) {
    if (target.resource.Dimension != dimension || (target.image.info.samples != VK_SAMPLE_COUNT_1_BIT) != multisampled) {
        error = "the view dimension does not match the texture's dimension or sample count";
        return false;
    }
    return true;
}

bool NoPlane(UINT plane, std::string& error) {
    if (plane != 0) {
        error = "plane slice " + std::to_string(plane) + " is not supported";
        return false;
    }
    return true;
}

ImageView Begin(const ViewFormat& format) {
    ImageView view;
    view.info.format = format.format;
    view.info.subresourceRange.aspectMask = format.aspect;
    view.info.subresourceRange.levelCount = 1;
    view.info.subresourceRange.layerCount = 1;
    view.info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                            format.alphaOne ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_IDENTITY};
    return view;
}

D3D12_SHADER_RESOURCE_VIEW_DESC DefaultShaderResourceView(const D3D12_RESOURCE_DESC& resource) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = resource.Format;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    const bool array = resource.DepthOrArraySize > 1;
    if (resource.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
        if (array) {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
            desc.Texture1DArray = {0, kAll, 0, resource.DepthOrArraySize, 0.0f};
        } else {
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
            desc.Texture1D = {0, kAll, 0.0f};
        }
    } else if (resource.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        desc.Texture3D = {0, kAll, 0.0f};
    } else if (resource.SampleDesc.Count > 1) {
        desc.ViewDimension = array ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D12_SRV_DIMENSION_TEXTURE2DMS;
        desc.Texture2DMSArray = {0, resource.DepthOrArraySize};
    } else if (array) {
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray = {0, kAll, 0, resource.DepthOrArraySize, 0, 0.0f};
    } else {
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Texture2D = {0, kAll, 0, 0.0f};
    }
    return desc;
}

}

std::optional<VkComponentMapping> ComponentMapping(UINT mapping, bool alphaOne) {
    if ((mapping & D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES) == 0 ||
        (mapping & ~((D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES << 1) - 1)) != 0) {
        return std::nullopt;
    }
    VkComponentSwizzle swizzles[4]{};
    for (unsigned component = 0; component < 4; ++component) {
        const UINT source = (mapping >> (3 * component)) & 7u;
        switch (source) {
        case 0: swizzles[component] = VK_COMPONENT_SWIZZLE_R; break;
        case 1: swizzles[component] = VK_COMPONENT_SWIZZLE_G; break;
        case 2: swizzles[component] = VK_COMPONENT_SWIZZLE_B; break;
        case 3: swizzles[component] = alphaOne ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_A; break;
        case 4: swizzles[component] = VK_COMPONENT_SWIZZLE_ZERO; break;
        case 5: swizzles[component] = VK_COMPONENT_SWIZZLE_ONE; break;
        default: return std::nullopt;
        }
    }
    return VkComponentMapping{swizzles[0], swizzles[1], swizzles[2], swizzles[3]};
}

std::optional<ImageView> ShaderResourceView(const D3D12_RESOURCE_DESC& resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* view, std::string& error) {
    const std::optional<ViewTarget> target = Target(resource, error);
    if (!target) {
        return std::nullopt;
    }
    if ((target->image.info.usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0) {
        error = "a shader resource view of a texture that denies shader resources";
        return std::nullopt;
    }
    const D3D12_SHADER_RESOURCE_VIEW_DESC desc = view != nullptr ? *view : DefaultShaderResourceView(resource);
    const std::optional<ViewFormat> format = ResolveFormat(*target, desc.Format, false, error);
    if (!format) {
        return std::nullopt;
    }
    ImageView result = Begin(*format);
    VkImageViewCreateInfo& info = result.info;
    const std::optional<VkComponentMapping> components = ComponentMapping(desc.Shader4ComponentMapping, format->alphaOne);
    if (!components) {
        error = "component mapping " + std::to_string(desc.Shader4ComponentMapping) + " is not valid";
        return std::nullopt;
    }
    info.components = *components;
    const auto clamp = [&error](FLOAT value) {
        if (value != 0.0f) {
            error = "a minimum LOD clamp is not supported";
            return false;
        }
        return true;
    };
    const UINT layers = target->image.info.arrayLayers;
    const auto stencilPlane = [&](UINT plane) {
        const UINT expected = format->aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? 1u : 0u;
        if (plane != expected) {
            error = "plane slice " + std::to_string(plane) + " does not match the view format";
            return false;
        }
        return true;
    };
    switch (desc.ViewDimension) {
    case D3D12_SRV_DIMENSION_TEXTURE1D:
        info.viewType = VK_IMAGE_VIEW_TYPE_1D;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE1D, false, error) || !SetMips(info, target->image, desc.Texture1D.MostDetailedMip, desc.Texture1D.MipLevels, error) ||
            !clamp(desc.Texture1D.ResourceMinLODClamp)) return std::nullopt;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
        info.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE1D, false, error) ||
            !SetMips(info, target->image, desc.Texture1DArray.MostDetailedMip, desc.Texture1DArray.MipLevels, error) ||
            !SetLayers(info, layers, desc.Texture1DArray.FirstArraySlice, desc.Texture1DArray.ArraySize, error) || !clamp(desc.Texture1DArray.ResourceMinLODClamp)) return std::nullopt;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2D:
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE2D, false, error) || !SetMips(info, target->image, desc.Texture2D.MostDetailedMip, desc.Texture2D.MipLevels, error) ||
            !stencilPlane(desc.Texture2D.PlaneSlice) || !clamp(desc.Texture2D.ResourceMinLODClamp)) return std::nullopt;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
        info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE2D, false, error) ||
            !SetMips(info, target->image, desc.Texture2DArray.MostDetailedMip, desc.Texture2DArray.MipLevels, error) ||
            !SetLayers(info, layers, desc.Texture2DArray.FirstArraySlice, desc.Texture2DArray.ArraySize, error) || !stencilPlane(desc.Texture2DArray.PlaneSlice) ||
            !clamp(desc.Texture2DArray.ResourceMinLODClamp)) return std::nullopt;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMS:
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE2D, true, error)) return std::nullopt;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
        info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE2D, true, error) ||
            !SetLayers(info, layers, desc.Texture2DMSArray.FirstArraySlice, desc.Texture2DMSArray.ArraySize, error)) return std::nullopt;
        break;
    case D3D12_SRV_DIMENSION_TEXTURE3D:
        info.viewType = VK_IMAGE_VIEW_TYPE_3D;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE3D, false, error) || !SetMips(info, target->image, desc.Texture3D.MostDetailedMip, desc.Texture3D.MipLevels, error) ||
            !clamp(desc.Texture3D.ResourceMinLODClamp)) return std::nullopt;
        break;
    case D3D12_SRV_DIMENSION_TEXTURECUBE:
        info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        if ((target->image.info.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) == 0) {
            error = "a cube view of a texture that is not a square array of six";
            return std::nullopt;
        }
        if (!SetMips(info, target->image, desc.TextureCube.MostDetailedMip, desc.TextureCube.MipLevels, error) || !SetLayers(info, layers, 0, 6, error) ||
            !clamp(desc.TextureCube.ResourceMinLODClamp)) return std::nullopt;
        break;
    case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
        info.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        if ((target->image.info.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) == 0) {
            error = "a cube view of a texture that is not a square array of six";
            return std::nullopt;
        }
        if (desc.TextureCubeArray.NumCubes == 0 || desc.TextureCubeArray.NumCubes > layers / 6 ||
            !SetMips(info, target->image, desc.TextureCubeArray.MostDetailedMip, desc.TextureCubeArray.MipLevels, error) ||
            !SetLayers(info, layers, desc.TextureCubeArray.First2DArrayFace, desc.TextureCubeArray.NumCubes * 6, error) || !clamp(desc.TextureCubeArray.ResourceMinLODClamp)) {
            if (error.empty()) error = std::to_string(desc.TextureCubeArray.NumCubes) + " cubes do not fit the texture";
            return std::nullopt;
        }
        result.features.insert("imageCubeArray");
        break;
    default:
        error = "shader resource view dimension " + std::to_string(static_cast<int>(desc.ViewDimension)) + " is not a texture view";
        return std::nullopt;
    }
    return result;
}

std::optional<ImageView> UnorderedAccessView(const D3D12_RESOURCE_DESC& resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC* view, std::string& error) {
    const std::optional<ViewTarget> target = Target(resource, error);
    if (!target) {
        return std::nullopt;
    }
    if ((target->image.info.usage & VK_IMAGE_USAGE_STORAGE_BIT) == 0) {
        error = "an unordered access view of a texture without ALLOW_UNORDERED_ACCESS";
        return std::nullopt;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    if (view != nullptr) {
        desc = *view;
    } else {
        desc.Format = resource.Format;
        if (resource.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
            desc.Texture1DArray = {0, 0, resource.DepthOrArraySize};
        } else if (resource.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            desc.Texture3D = {0, 0, kAll};
        } else {
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray = {0, 0, resource.DepthOrArraySize, 0};
        }
    }
    const std::optional<ViewFormat> format = ResolveFormat(*target, desc.Format, false, error);
    if (!format) {
        return std::nullopt;
    }
    ImageView result = Begin(*format);
    VkImageViewCreateInfo& info = result.info;
    const UINT layers = target->image.info.arrayLayers;
    switch (desc.ViewDimension) {
    case D3D12_UAV_DIMENSION_TEXTURE1D:
        info.viewType = VK_IMAGE_VIEW_TYPE_1D;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE1D, false, error) || !SetMips(info, target->image, desc.Texture1D.MipSlice, 1, error)) return std::nullopt;
        break;
    case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
        info.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE1D, false, error) || !SetMips(info, target->image, desc.Texture1DArray.MipSlice, 1, error) ||
            !SetLayers(info, layers, desc.Texture1DArray.FirstArraySlice, desc.Texture1DArray.ArraySize, error)) return std::nullopt;
        break;
    case D3D12_UAV_DIMENSION_TEXTURE2D:
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE2D, false, error) || !SetMips(info, target->image, desc.Texture2D.MipSlice, 1, error) ||
            !NoPlane(desc.Texture2D.PlaneSlice, error)) return std::nullopt;
        break;
    case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
        info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE2D, false, error) || !SetMips(info, target->image, desc.Texture2DArray.MipSlice, 1, error) ||
            !SetLayers(info, layers, desc.Texture2DArray.FirstArraySlice, desc.Texture2DArray.ArraySize, error) || !NoPlane(desc.Texture2DArray.PlaneSlice, error)) return std::nullopt;
        break;
    case D3D12_UAV_DIMENSION_TEXTURE3D: {
        info.viewType = VK_IMAGE_VIEW_TYPE_3D;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE3D, false, error) || !SetMips(info, target->image, desc.Texture3D.MipSlice, 1, error)) return std::nullopt;
        const std::uint32_t depth = (std::max)(1u, target->image.info.extent.depth >> desc.Texture3D.MipSlice);
        if (desc.Texture3D.FirstWSlice != 0 || (desc.Texture3D.WSize != kAll && desc.Texture3D.WSize != depth)) {
            error = "an unordered access view of part of a 3D texture's depth is not supported";
            return std::nullopt;
        }
        break;
    }
    default:
        error = "unordered access view dimension " + std::to_string(static_cast<int>(desc.ViewDimension)) + " is not a supported texture view";
        return std::nullopt;
    }
    return result;
}

std::optional<ImageView> RenderTargetView(const D3D12_RESOURCE_DESC& resource, const D3D12_RENDER_TARGET_VIEW_DESC* view, std::string& error) {
    const std::optional<ViewTarget> target = Target(resource, error);
    if (!target) {
        return std::nullopt;
    }
    if ((target->image.info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
        error = "a render target view of a texture without ALLOW_RENDER_TARGET";
        return std::nullopt;
    }
    D3D12_RENDER_TARGET_VIEW_DESC desc{};
    if (view != nullptr) {
        desc = *view;
    } else {
        desc.Format = resource.Format;
        if (resource.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
            desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
            desc.Texture1DArray = {0, 0, resource.DepthOrArraySize};
        } else if (resource.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
            desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
            desc.Texture3D = {0, 0, kAll};
        } else if (resource.SampleDesc.Count > 1) {
            desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
            desc.Texture2DMSArray = {0, resource.DepthOrArraySize};
        } else {
            desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray = {0, 0, resource.DepthOrArraySize, 0};
        }
    }
    const std::optional<ViewFormat> format = ResolveFormat(*target, desc.Format, false, error);
    if (!format) {
        return std::nullopt;
    }
    ImageView result = Begin(*format);
    VkImageViewCreateInfo& info = result.info;
    info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    const UINT layers = target->image.info.arrayLayers;
    switch (desc.ViewDimension) {
    case D3D12_RTV_DIMENSION_TEXTURE1D:
        info.viewType = VK_IMAGE_VIEW_TYPE_1D;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE1D, false, error) || !SetMips(info, target->image, desc.Texture1D.MipSlice, 1, error)) return std::nullopt;
        break;
    case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
        info.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE1D, false, error) || !SetMips(info, target->image, desc.Texture1DArray.MipSlice, 1, error) ||
            !SetLayers(info, layers, desc.Texture1DArray.FirstArraySlice, desc.Texture1DArray.ArraySize, error)) return std::nullopt;
        break;
    case D3D12_RTV_DIMENSION_TEXTURE2D:
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE2D, false, error) || !SetMips(info, target->image, desc.Texture2D.MipSlice, 1, error) ||
            !NoPlane(desc.Texture2D.PlaneSlice, error)) return std::nullopt;
        break;
    case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
        info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE2D, false, error) || !SetMips(info, target->image, desc.Texture2DArray.MipSlice, 1, error) ||
            !SetLayers(info, layers, desc.Texture2DArray.FirstArraySlice, desc.Texture2DArray.ArraySize, error) || !NoPlane(desc.Texture2DArray.PlaneSlice, error)) return std::nullopt;
        break;
    case D3D12_RTV_DIMENSION_TEXTURE2DMS:
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE2D, true, error)) return std::nullopt;
        break;
    case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
        info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE2D, true, error) ||
            !SetLayers(info, layers, desc.Texture2DMSArray.FirstArraySlice, desc.Texture2DMSArray.ArraySize, error)) return std::nullopt;
        break;
    case D3D12_RTV_DIMENSION_TEXTURE3D: {
        info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE3D, false, error) || !SetMips(info, target->image, desc.Texture3D.MipSlice, 1, error)) return std::nullopt;
        const std::uint32_t depth = (std::max)(1u, target->image.info.extent.depth >> desc.Texture3D.MipSlice);
        if (!SetLayers(info, depth, desc.Texture3D.FirstWSlice, desc.Texture3D.WSize, error)) return std::nullopt;
        break;
    }
    default:
        error = "render target view dimension " + std::to_string(static_cast<int>(desc.ViewDimension)) + " is not a supported texture view";
        return std::nullopt;
    }
    return result;
}

std::optional<ImageView> DepthStencilView(const D3D12_RESOURCE_DESC& resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* view, std::string& error) {
    const std::optional<ViewTarget> target = Target(resource, error);
    if (!target) {
        return std::nullopt;
    }
    if ((target->image.info.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0) {
        error = "a depth-stencil view of a texture without ALLOW_DEPTH_STENCIL";
        return std::nullopt;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
    if (view != nullptr) {
        desc = *view;
    } else {
        desc.Format = resource.Format;
        if (resource.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) {
            desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
            desc.Texture1DArray = {0, 0, resource.DepthOrArraySize};
        } else if (resource.SampleDesc.Count > 1) {
            desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
            desc.Texture2DMSArray = {0, resource.DepthOrArraySize};
        } else {
            desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray = {0, 0, resource.DepthOrArraySize};
        }
    }
    const UINT flagBits = static_cast<UINT>(desc.Flags);
    if ((flagBits & ~static_cast<UINT>(D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL)) != 0) {
        error = "depth-stencil view flags " + std::to_string(flagBits) + " are unknown";
        return std::nullopt;
    }
    const std::optional<ViewFormat> format = ResolveFormat(*target, desc.Format, true, error);
    if (!format) {
        return std::nullopt;
    }
    ImageView result = Begin(*format);
    VkImageViewCreateInfo& info = result.info;
    const UINT layers = target->image.info.arrayLayers;
    switch (desc.ViewDimension) {
    case D3D12_DSV_DIMENSION_TEXTURE1D:
        info.viewType = VK_IMAGE_VIEW_TYPE_1D;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE1D, false, error) || !SetMips(info, target->image, desc.Texture1D.MipSlice, 1, error)) return std::nullopt;
        break;
    case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
        info.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE1D, false, error) || !SetMips(info, target->image, desc.Texture1DArray.MipSlice, 1, error) ||
            !SetLayers(info, layers, desc.Texture1DArray.FirstArraySlice, desc.Texture1DArray.ArraySize, error)) return std::nullopt;
        break;
    case D3D12_DSV_DIMENSION_TEXTURE2D:
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE2D, false, error) || !SetMips(info, target->image, desc.Texture2D.MipSlice, 1, error)) return std::nullopt;
        break;
    case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
        info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE2D, false, error) || !SetMips(info, target->image, desc.Texture2DArray.MipSlice, 1, error) ||
            !SetLayers(info, layers, desc.Texture2DArray.FirstArraySlice, desc.Texture2DArray.ArraySize, error)) return std::nullopt;
        break;
    case D3D12_DSV_DIMENSION_TEXTURE2DMS:
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE2D, true, error)) return std::nullopt;
        break;
    case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
        info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if (!ExpectDimension(*target, D3D12_RESOURCE_DIMENSION_TEXTURE2D, true, error) ||
            !SetLayers(info, layers, desc.Texture2DMSArray.FirstArraySlice, desc.Texture2DMSArray.ArraySize, error)) return std::nullopt;
        break;
    default:
        error = "depth-stencil view dimension " + std::to_string(static_cast<int>(desc.ViewDimension)) + " is unknown";
        return std::nullopt;
    }
    return result;
}

std::optional<Sampler> TranslateSampler(const D3D12_SAMPLER_DESC& desc, std::string& error) {
    Sampler sampler;
    VkSamplerCreateInfo& info = sampler.info;
    const UINT filter = static_cast<UINT>(desc.Filter);
    if ((filter & ~0x1D5u) != 0) {
        error = "filter " + std::to_string(filter) + " is unknown";
        return std::nullopt;
    }
    const UINT reduction = (filter >> D3D12_FILTER_REDUCTION_TYPE_SHIFT) & D3D12_FILTER_REDUCTION_TYPE_MASK;
    const auto filterOf = [](UINT bits) { return bits == D3D12_FILTER_TYPE_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; };
    info.minFilter = filterOf((filter >> D3D12_MIN_FILTER_SHIFT) & D3D12_FILTER_TYPE_MASK);
    info.magFilter = filterOf((filter >> D3D12_MAG_FILTER_SHIFT) & D3D12_FILTER_TYPE_MASK);
    info.mipmapMode = ((filter >> D3D12_MIP_FILTER_SHIFT) & D3D12_FILTER_TYPE_MASK) == D3D12_FILTER_TYPE_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if ((filter & D3D12_ANISOTROPIC_FILTERING_BIT) != 0) {
        if (desc.MaxAnisotropy < 1 || desc.MaxAnisotropy > 16) {
            error = "anisotropy " + std::to_string(desc.MaxAnisotropy) + " is outside 1 to 16";
            return std::nullopt;
        }
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = static_cast<float>(desc.MaxAnisotropy);
        info.minFilter = VK_FILTER_LINEAR;
        info.magFilter = VK_FILTER_LINEAR;
        sampler.features.insert("samplerAnisotropy");
    }
    if (reduction == D3D12_FILTER_REDUCTION_TYPE_COMPARISON) {
        const std::optional<VkCompareOp> compare = VulkanCompareOp(desc.ComparisonFunc);
        if (!compare) {
            error = "comparison " + std::to_string(static_cast<int>(desc.ComparisonFunc)) + " is unknown";
            return std::nullopt;
        }
        info.compareEnable = VK_TRUE;
        info.compareOp = *compare;
    } else if (reduction == D3D12_FILTER_REDUCTION_TYPE_MINIMUM || reduction == D3D12_FILTER_REDUCTION_TYPE_MAXIMUM) {
        sampler.reduction.reductionMode = reduction == D3D12_FILTER_REDUCTION_TYPE_MINIMUM ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX;
        sampler.usesReduction = true;
        sampler.features.insert("samplerFilterMinmax");
    }
    bool border = false;
    const D3D12_TEXTURE_ADDRESS_MODE modes[3] = {desc.AddressU, desc.AddressV, desc.AddressW};
    VkSamplerAddressMode* targets[3] = {&info.addressModeU, &info.addressModeV, &info.addressModeW};
    for (int axis = 0; axis < 3; ++axis) {
        switch (modes[axis]) {
        case D3D12_TEXTURE_ADDRESS_MODE_WRAP: *targets[axis] = VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
        case D3D12_TEXTURE_ADDRESS_MODE_MIRROR: *targets[axis] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        case D3D12_TEXTURE_ADDRESS_MODE_CLAMP: *targets[axis] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case D3D12_TEXTURE_ADDRESS_MODE_BORDER:
            *targets[axis] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            border = true;
            break;
        case D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE:
            *targets[axis] = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
            sampler.features.insert("samplerMirrorClampToEdge");
            break;
        default:
            error = "address mode " + std::to_string(static_cast<int>(modes[axis])) + " is unknown";
            return std::nullopt;
        }
    }
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (border) {
        const FLOAT* c = desc.BorderColor;
        if (c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == 0.0f) {
            info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        } else if (c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == 1.0f) {
            info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        } else if (c[0] == 1.0f && c[1] == 1.0f && c[2] == 1.0f && c[3] == 1.0f) {
            info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        } else {
            error = "a border color other than transparent black, opaque black or opaque white is not supported";
            return std::nullopt;
        }
    }
    if (!std::isfinite(desc.MipLODBias) || std::isnan(desc.MinLOD) || std::isnan(desc.MaxLOD) || desc.MinLOD > desc.MaxLOD) {
        error = "the sampler's LOD bias or range is not valid";
        return std::nullopt;
    }
    info.mipLodBias = desc.MipLODBias;
    info.minLod = desc.MinLOD < 0.0f ? 0.0f : desc.MinLOD;
    info.maxLod = desc.MaxLOD >= D3D12_FLOAT32_MAX ? VK_LOD_CLAMP_NONE : desc.MaxLOD;
    if (info.maxLod < info.minLod) {
        info.maxLod = info.minLod;
    }
    return sampler;
}

}
