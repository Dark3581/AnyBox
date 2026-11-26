#include "FrameDump.hpp"

#include "AbxApi.hpp"
#include "BindlessSet.hpp"
#include "DescriptorHeap.hpp"
#include "DescriptorWriter.hpp"
#include "IndirectClamp.hpp"
#include "LiveStats.hpp"
#include "Resource.hpp"
#include "TitleAddressSpace.hpp"
#include "TitleTextures.hpp"
#include "VulkanDevice.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace abx::d3d12x {

namespace {

std::mutex g_lock;
std::set<Resource*> g_targets;
std::set<Resource*> g_depths;
std::set<Resource*> g_inputs;
std::set<Resource*> g_reads;
std::set<Resource*> g_watched;
std::set<std::uint32_t> g_tables;
std::string g_bufferShader;
std::set<std::pair<std::uint64_t, std::uint64_t>> g_bufferFiles;

bool FourBytes(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return true;
    default: return false;
    }
}

std::uint32_t ExtraTexelBytes(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_UNORM: return 1;
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R8G8_UNORM: return 2;
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT: return 4;
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT: return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    default: return 0;
    }
}

enum class DepthBits { None, Float32, Unorm24, Unorm16 };

DepthBits DepthLayout(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT: return DepthBits::Float32;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return DepthBits::Unorm24;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM: return DepthBits::Unorm16;
    default: return DepthBits::None;
    }
}

float DepthValue(DepthBits layout, const std::uint8_t* texel) {
    float value = 0.0f;
    std::uint32_t word = 0;
    std::uint16_t half = 0;
    switch (layout) {
    case DepthBits::Float32: std::memcpy(&value, texel, 4); break;
    case DepthBits::Unorm24: std::memcpy(&word, texel, 4); value = static_cast<float>(word & 0xFFFFFFu) / 16777215.0f; break;
    case DepthBits::Unorm16: std::memcpy(&half, texel, 2); value = static_cast<float>(half) / 65535.0f; break;
    default: break;
    }
    return value;
}

std::uint32_t TexelBytes(DXGI_FORMAT format) {
    if (FourBytes(format) || format == DXGI_FORMAT_R11G11B10_FLOAT || format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP) return 4;
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) return 8;
    return ExtraTexelBytes(format);
}

float SmallFloat(std::uint32_t bits, std::uint32_t mantissaBits) {
    const std::uint32_t mantissa = bits & ((1u << mantissaBits) - 1);
    const std::uint32_t exponent = bits >> mantissaBits;
    const float fraction = static_cast<float>(mantissa) / static_cast<float>(1u << mantissaBits);
    if (exponent == 31) return mantissa == 0 ? (std::numeric_limits<float>::infinity)() : std::numeric_limits<float>::quiet_NaN();
    if (exponent == 0) return std::ldexp(fraction, -14);
    return std::ldexp(1.0f + fraction, static_cast<int>(exponent) - 15);
}

float HalfFloat(std::uint16_t bits) {
    const float value = SmallFloat(bits & 0x7FFFu, 10);
    return (bits & 0x8000u) != 0 ? -value : value;
}

void ExtraValues(DXGI_FORMAT format, const std::uint8_t* texel, float values[3]) {
    values[0] = values[1] = values[2] = 0.0f;
    std::uint16_t halves[2] = {};
    std::uint32_t words[4] = {};
    float floats[4] = {};
    switch (format) {
    case DXGI_FORMAT_R8_UNORM: values[0] = values[1] = values[2] = static_cast<float>(texel[0]) / 255.0f; break;
    case DXGI_FORMAT_R8_UINT: values[0] = values[1] = values[2] = static_cast<float>(texel[0]) / 255.0f + (texel[0] != 0 ? 0.25f : 0.0f); break;
    case DXGI_FORMAT_R16_UINT:
        std::memcpy(halves, texel, 2);
        values[0] = values[1] = values[2] = static_cast<float>(halves[0] & 0xFFu) / 255.0f + (halves[0] != 0 ? 0.25f : 0.0f);
        break;
    case DXGI_FORMAT_R8G8_UNORM: values[0] = static_cast<float>(texel[0]) / 255.0f; values[1] = static_cast<float>(texel[1]) / 255.0f; break;
    case DXGI_FORMAT_R16_FLOAT: std::memcpy(halves, texel, 2); values[0] = values[1] = values[2] = HalfFloat(halves[0]); break;
    case DXGI_FORMAT_R16_UNORM: std::memcpy(halves, texel, 2); values[0] = values[1] = values[2] = static_cast<float>(halves[0]) / 65535.0f; break;
    case DXGI_FORMAT_R16G16_UNORM:
        std::memcpy(halves, texel, 4);
        values[0] = static_cast<float>(halves[0]) / 65535.0f;
        values[1] = static_cast<float>(halves[1]) / 65535.0f;
        break;
    case DXGI_FORMAT_R16G16_FLOAT: std::memcpy(halves, texel, 4); values[0] = HalfFloat(halves[0]); values[1] = HalfFloat(halves[1]); break;
    case DXGI_FORMAT_R32_FLOAT: std::memcpy(floats, texel, 4); values[0] = values[1] = values[2] = floats[0]; break;
    case DXGI_FORMAT_R32G32_FLOAT: std::memcpy(floats, texel, 8); values[0] = floats[0]; values[1] = floats[1]; break;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: std::memcpy(floats, texel, 16); values[0] = floats[0]; values[1] = floats[1]; values[2] = floats[2]; break;
    case DXGI_FORMAT_R32_UINT: std::memcpy(words, texel, 4); values[0] = values[1] = values[2] = static_cast<float>(words[0] & 0xFFu) / 255.0f + (words[0] != 0 ? 0.25f : 0.0f); break;
    case DXGI_FORMAT_R32G32_UINT:
        std::memcpy(words, texel, 8);
        values[0] = static_cast<float>(words[0] & 0xFFu) / 255.0f + (words[0] != 0 ? 0.25f : 0.0f);
        values[1] = static_cast<float>(words[1] & 0xFFu) / 255.0f + (words[1] != 0 ? 0.25f : 0.0f);
        break;
    default: break;
    }
}

void SharedExponent(std::uint32_t packed, float values[3]) {
    const int exponent = static_cast<int>(packed >> 27) - 24;
    values[0] = std::ldexp(static_cast<float>(packed & 0x1FFu), exponent);
    values[1] = std::ldexp(static_cast<float>((packed >> 9) & 0x1FFu), exponent);
    values[2] = std::ldexp(static_cast<float>((packed >> 18) & 0x1FFu), exponent);
}

std::uint8_t Shown(float value) {
    if (!(value > 0.0f)) return 0;
    if (std::isinf(value)) return 255;
    return static_cast<std::uint8_t>(std::pow(value / (1.0f + value), 1.0f / 2.2f) * 255.0f + 0.5f);
}

void Dump(const VulkanDevice& device, Resource* resource, const std::string& path, bool depth = false) {
    const TextureRef& texture = resource->Texture();
    const D3D12_RESOURCE_DESC& desc = texture.desc;
    const DepthBits depthLayout = depth ? DepthLayout(desc.Format) : DepthBits::None;
    const std::uint32_t texelBytes = !depth ? TexelBytes(desc.Format) : depthLayout == DepthBits::None ? 0u : depthLayout == DepthBits::Unorm16 ? 2u : 4u;
    if (texture.image == VK_NULL_HANDLE || texelBytes == 0 || desc.Width == 0 || desc.Height == 0 || desc.SampleDesc.Count > 1) {
        AbxLog("d3d12_x", "frame dump: %s skipped (%llux%u, format %d, %u samples: not a single-sample color format the dump reads)", path.c_str(), static_cast<unsigned long long>(desc.Width), desc.Height,
               static_cast<int>(desc.Format), desc.SampleDesc.Count);
        return;
    }
    const std::uint32_t width = static_cast<std::uint32_t>(desc.Width);
    const std::uint32_t height = desc.Height;
    const std::uint32_t slices = !depth && desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && path.find("_watch") != std::string::npos ? (std::max)(std::uint32_t{desc.DepthOrArraySize}, 1u) : 1u;
    std::string error;
    DeviceBuffer staging;
    if (!CreateHostBuffer(device, static_cast<VkDeviceSize>(width) * height * texelBytes * slices, VK_BUFFER_USAGE_TRANSFER_DST_BIT, staging, error)) {
        AbxLog("d3d12_x", "frame dump: %s failed: %s", path.c_str(), error.c_str());
        return;
    }
    const bool copied = device.Submit(
        [&](VkCommandBuffer commands) {
            VkBufferImageCopy region{};
            region.imageSubresource = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {width, height, slices};
            device.Vk().cmdCopyImageToBuffer(commands, texture.image, VK_IMAGE_LAYOUT_GENERAL, staging.buffer, 1, &region);
        },
        error);
    if (!copied) {
        AbxLog("d3d12_x", "frame dump: %s failed: %s", path.c_str(), error.c_str());
        DestroyBuffer(device, staging);
        return;
    }
    const std::uint64_t texelData = static_cast<std::uint64_t>(width) * height * texelBytes;
    if (slices > 1) {
        const std::string rawPath = path + ".volume.raw";
        if (std::FILE* raw = std::fopen(rawPath.c_str(), "wb")) {
            std::fwrite(staging.mapped, 1, static_cast<std::size_t>(texelData * slices), raw);
            std::fclose(raw);
            AbxLog("d3d12_x", "frame dump: %s holds every texel of mip 0 of a 3D texture as the GPU has it (%ux%ux%u, format %d, %u byte(s) each, slice after slice, rows top to bottom); "
                              "the picture shows slice 0",
                   rawPath.c_str(), width, height, slices, static_cast<int>(desc.Format), texelBytes);
        }
    }
    if (!depth && (width < 8 || height < 8)) {
        const std::uint64_t shown = (std::min)(texelData, std::uint64_t{32}) & ~std::uint64_t{3};
        const auto wordsText = [shown](const std::uint8_t* bytes) {
            std::string words;
            for (std::uint64_t at = 0; at < shown; at += 4) {
                std::uint32_t word = 0;
                std::memcpy(&word, bytes + at, 4);
                char item[16];
                std::snprintf(item, sizeof(item), " %08x", word);
                words += item;
            }
            return words;
        };
        const std::uint64_t address = resource->TextureAddress();
        std::string title = " nothing the CPU can read";
        MEMORY_BASIC_INFORMATION region{};
        if (address != 0 && shown != 0 && VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) == sizeof(region) && region.State == MEM_COMMIT &&
            (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0 && reinterpret_cast<std::uint64_t>(region.BaseAddress) + region.RegionSize >= address + shown) {
            title = wordsText(reinterpret_cast<const std::uint8_t*>(address));
        }
        AbxLog("d3d12_x", "frame dump: %s is small (%ux%u, format %d): the GPU image starts with%s; title memory at 0x%llx starts with%s", path.c_str(), width, height,
               static_cast<int>(desc.Format), wordsText(static_cast<const std::uint8_t*>(staging.mapped)).c_str(), static_cast<unsigned long long>(address), title.c_str());
    }
    if (!depth && path.find("_watch") != std::string::npos &&
        (desc.Format == DXGI_FORMAT_R32_UINT || desc.Format == DXGI_FORMAT_R16_UINT || desc.Format == DXGI_FORMAT_R8_UINT)) {
        const std::string rawPath = path + ".raw";
        if (std::FILE* raw = std::fopen(rawPath.c_str(), "wb")) {
            std::fwrite(staging.mapped, 1, static_cast<std::size_t>(texelData), raw);
            std::fclose(raw);
            AbxLog("d3d12_x", "frame dump: %s holds every texel of mip 0 as the GPU has it (%ux%u, %u byte(s) each, rows top to bottom)", rawPath.c_str(), width, height,
                   texelBytes);
        }
    }
    constexpr std::uint32_t kStep = 4;
    const std::uint32_t outWidth = width / kStep;
    const std::uint32_t outHeight = height / kStep;
    const std::uint32_t rowBytes = (outWidth * 3 + 3) & ~3u;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(rowBytes) * outHeight);
    const auto* source = static_cast<const std::uint8_t*>(staging.mapped);
    const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
                      desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
    const bool packedFloat = desc.Format == DXGI_FORMAT_R11G11B10_FLOAT;
    const bool sharedExponent = desc.Format == DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    const bool wordTexels = desc.Format == DXGI_FORMAT_R32_UINT;
    const bool smallWords = desc.Format == DXGI_FORMAT_R8_UINT || desc.Format == DXGI_FORMAT_R16_UINT;
    std::uint64_t smallNonzero = 0;
    std::uint32_t smallLowest = 0xFFFFu;
    std::uint32_t smallHighest = 0;
    std::uint64_t wordLit = 0;
    std::uint64_t wordNonzero = 0;
    float wordLargest = 0.0f;
    const bool halfFloat = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    const bool tenBits = desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM || desc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS;
    const bool extra = ExtraTexelBytes(desc.Format) != 0;
    std::uint64_t lit = 0;
    std::uint64_t notFinite = 0;
    float largest = 0.0f;
    std::uint64_t depthAtZero = 0;
    std::uint64_t depthAtOne = 0;
    std::uint64_t depthBetween = 0;
    float depthLowest = 1.0f;
    float depthHighest = 0.0f;
    for (std::uint32_t y = 0; y < outHeight; ++y) {
        for (std::uint32_t x = 0; x < outWidth; ++x) {
            const std::uint8_t* texel = source + (static_cast<std::size_t>(y) * kStep * width + static_cast<std::size_t>(x) * kStep) * texelBytes;
            std::uint8_t* out = pixels.data() + static_cast<std::size_t>(outHeight - 1 - y) * rowBytes + static_cast<std::size_t>(x) * 3;
            std::uint8_t red = 0;
            std::uint8_t green = 0;
            std::uint8_t blue = 0;
            if (depth) {
                const float value = DepthValue(depthLayout, texel);
                if (!(value > 0.0f)) {
                    ++depthAtZero;
                } else if (value >= 1.0f) {
                    ++depthAtOne;
                    red = green = blue = 255;
                } else {
                    ++depthBetween;
                    depthLowest = (std::min)(depthLowest, value);
                    depthHighest = (std::max)(depthHighest, value);
                    red = green = blue = static_cast<std::uint8_t>(40.0f + 200.0f * std::pow(value, 0.2f));
                }
            } else if (packedFloat || halfFloat || extra || sharedExponent) {
                float values[3] = {};
                if (wordTexels) {
                    std::uint32_t packed = 0;
                    std::memcpy(&packed, texel, sizeof(packed));
                    float colour[3] = {};
                    SharedExponent(packed, colour);
                    if (packed != 0) ++wordNonzero;
                    if (static_cast<unsigned>(Shown(colour[0])) + Shown(colour[1]) + Shown(colour[2]) > 30) ++wordLit;
                    for (const float value : colour) wordLargest = (std::max)(wordLargest, value);
                }
                if (smallWords) {
                    std::uint16_t small = 0;
                    std::memcpy(&small, texel, texelBytes);
                    if (small != 0) ++smallNonzero;
                    smallLowest = (std::min)(smallLowest, static_cast<std::uint32_t>(small));
                    smallHighest = (std::max)(smallHighest, static_cast<std::uint32_t>(small));
                }
                if (sharedExponent) {
                    std::uint32_t packed = 0;
                    std::memcpy(&packed, texel, sizeof(packed));
                    SharedExponent(packed, values);
                } else if (extra) {
                    ExtraValues(desc.Format, texel, values);
                } else if (packedFloat) {
                    std::uint32_t packed = 0;
                    std::memcpy(&packed, texel, sizeof(packed));
                    values[0] = SmallFloat(packed & 0x7FFu, 6);
                    values[1] = SmallFloat((packed >> 11) & 0x7FFu, 6);
                    values[2] = SmallFloat(packed >> 22, 5);
                } else {
                    std::uint16_t halves[3] = {};
                    std::memcpy(halves, texel, sizeof(halves));
                    for (int channel = 0; channel < 3; ++channel) values[channel] = HalfFloat(halves[channel]);
                }
                bool finite = true;
                for (const float value : values) {
                    if (!std::isfinite(value)) finite = false;
                    else largest = (std::max)(largest, value);
                }
                if (!finite) ++notFinite;
                red = Shown(values[0]);
                green = Shown(values[1]);
                blue = Shown(values[2]);
            } else if (tenBits) {
                std::uint32_t packed = 0;
                std::memcpy(&packed, texel, sizeof(packed));
                red = static_cast<std::uint8_t>((packed & 0x3FFu) >> 2);
                green = static_cast<std::uint8_t>(((packed >> 10) & 0x3FFu) >> 2);
                blue = static_cast<std::uint8_t>(((packed >> 20) & 0x3FFu) >> 2);
            } else {
                red = bgra ? texel[2] : texel[0];
                green = texel[1];
                blue = bgra ? texel[0] : texel[2];
            }
            out[0] = blue;
            out[1] = green;
            out[2] = red;
            if (static_cast<unsigned>(red) + green + blue > 30) ++lit;
        }
    }
    DestroyBuffer(device, staging);
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return;
    const std::uint32_t imageBytes = static_cast<std::uint32_t>(pixels.size());
    const std::uint32_t fileBytes = 54 + imageBytes;
    std::uint8_t header[54] = {'B', 'M'};
    std::memcpy(header + 2, &fileBytes, 4);
    const std::uint32_t offset = 54, infoBytes = 40, planesBits = 1u | (24u << 16);
    std::memcpy(header + 10, &offset, 4);
    std::memcpy(header + 14, &infoBytes, 4);
    std::memcpy(header + 18, &outWidth, 4);
    std::memcpy(header + 22, &outHeight, 4);
    std::memcpy(header + 26, &planesBits, 4);
    std::memcpy(header + 34, &imageBytes, 4);
    std::fwrite(header, 1, sizeof(header), file);
    std::fwrite(pixels.data(), 1, pixels.size(), file);
    std::fclose(file);
    char floats[160] = {};
    if (depth) {
        std::snprintf(floats, sizeof(floats), "; depth of the sampled pixels: %llu at 0, %llu at 1, %llu between (%g to %g)", static_cast<unsigned long long>(depthAtZero),
                      static_cast<unsigned long long>(depthAtOne), static_cast<unsigned long long>(depthBetween), static_cast<double>(depthBetween != 0 ? depthLowest : 0.0f),
                      static_cast<double>(depthHighest));
    } else if (packedFloat || halfFloat || sharedExponent) {
        std::snprintf(floats, sizeof(floats), ", %llu with a value that is not finite, largest finite value %g", static_cast<unsigned long long>(notFinite),
                      static_cast<double>(largest));
    } else if (wordTexels) {
        std::snprintf(floats, sizeof(floats), "; %llu words not zero; read as shared-exponent colour %llu not dark, largest value %g", static_cast<unsigned long long>(wordNonzero),
                      static_cast<unsigned long long>(wordLit), static_cast<double>(wordLargest));
    } else if (smallWords) {
        std::snprintf(floats, sizeof(floats), "; %llu values not zero, lowest 0x%x, highest 0x%x", static_cast<unsigned long long>(smallNonzero), smallLowest, smallHighest);
    }
    AbxLog("d3d12_x", "frame dump: %s (%ux%u, format %d, %llu of %u sampled pixels not dark%s)", path.c_str(), width, height, static_cast<int>(desc.Format),
           static_cast<unsigned long long>(lit), outWidth * outHeight, floats);
}

void DumpFloatLevels(const VulkanDevice& device, Resource* resource, const std::string& path, const std::string& kept) {
    const TextureRef& texture = resource->Texture();
    const D3D12_RESOURCE_DESC& desc = texture.desc;
    const std::uint32_t levels = texture.layout.info.mipLevels;
    if (texture.image == VK_NULL_HANDLE || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count > 1 || desc.Width == 0 || desc.Height == 0 || levels == 0) {
        AbxLog("d3d12_x", "frame dump: input %s skipped (%llux%u, format %d, %u samples: not a single-sample 2D texture)", path.c_str(), static_cast<unsigned long long>(desc.Width),
               desc.Height, static_cast<int>(desc.Format), desc.SampleDesc.Count);
        return;
    }
    const VkImageAspectFlags aspect = (texture.layout.format.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    AbxLog("d3d12_x", "frame dump: input %s (%llux%u, format %d, %u mips, flags 0x%x) read by a traced dispatch is %s", path.c_str(), static_cast<unsigned long long>(desc.Width),
           desc.Height, static_cast<int>(desc.Format), levels, static_cast<unsigned>(desc.Flags), kept.c_str());
    std::vector<float> previous;
    std::uint32_t previousWidth = 0;
    std::uint32_t previousHeight = 0;
    for (std::uint32_t level = 0; level < levels; ++level) {
        const std::uint32_t width = (std::max)(static_cast<std::uint32_t>(desc.Width >> level), 1u);
        const std::uint32_t height = (std::max)(desc.Height >> level, 1u);
        std::string error;
        DeviceBuffer staging;
        if (!CreateHostBuffer(device, static_cast<VkDeviceSize>(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, staging, error)) {
            AbxLog("d3d12_x", "frame dump: input %s mip %u failed: %s", path.c_str(), level, error.c_str());
            return;
        }
        const bool copied = device.Submit(
            [&](VkCommandBuffer commands) {
                VkBufferImageCopy region{};
                region.imageSubresource = {aspect, level, 0, 1};
                region.imageExtent = {width, height, 1};
                device.Vk().cmdCopyImageToBuffer(commands, texture.image, VK_IMAGE_LAYOUT_GENERAL, staging.buffer, 1, &region);
            },
            error);
        if (!copied) {
            AbxLog("d3d12_x", "frame dump: input %s mip %u failed: %s", path.c_str(), level, error.c_str());
            DestroyBuffer(device, staging);
            return;
        }
        const auto* source = static_cast<const std::uint8_t*>(staging.mapped);
        if (desc.Width <= 256 && desc.Height <= 256) {
            const std::string raw = path.substr(0, path.size() - 4) + "_mip" + std::to_string(level) + ".f32";
            if (std::FILE* file = std::fopen(raw.c_str(), "wb")) {
                std::fwrite(source, 4, static_cast<std::size_t>(width) * height, file);
                std::fclose(file);
            }
        }
        std::uint64_t atZero = 0;
        std::uint64_t below = 0;
        std::uint64_t between = 0;
        std::uint64_t above = 0;
        std::uint64_t notFinite = 0;
        float lowest = 1.0f;
        float highest = 0.0f;
        float largest = 0.0f;
        float least = 0.0f;
        const auto valueAt = [&](std::uint32_t x, std::uint32_t y) {
            float value = 0.0f;
            std::memcpy(&value, source + (static_cast<std::size_t>(y) * width + x) * 4, 4);
            return value;
        };
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const float value = valueAt(x, y);
                if (!std::isfinite(value)) {
                    ++notFinite;
                } else if (value == 0.0f) {
                    ++atZero;
                } else if (value < 0.0f) {
                    ++below;
                    least = (std::min)(least, value);
                } else if (value >= 1.0f) {
                    ++above;
                    largest = (std::max)(largest, value);
                } else {
                    ++between;
                    lowest = (std::min)(lowest, value);
                    highest = (std::max)(highest, value);
                }
            }
        }
        AbxLog("d3d12_x", "frame dump: input %s mip %u (%ux%u): %llu at 0, %llu between 0 and 1 (%g to %g), %llu at 1 or above (up to %g), %llu below 0 (down to %g), %llu not finite",
               path.c_str(), level, width, height, static_cast<unsigned long long>(atZero), static_cast<unsigned long long>(between),
               static_cast<double>(between != 0 ? lowest : 0.0f), static_cast<double>(highest), static_cast<unsigned long long>(above), static_cast<double>(largest),
               static_cast<unsigned long long>(below), static_cast<double>(least), static_cast<unsigned long long>(notFinite));
        if (level != 0 && previous.size() == static_cast<std::size_t>(previousWidth) * previousHeight && !previous.empty()) {
            std::uint64_t asMinimum = 0;
            std::uint64_t asMaximum = 0;
            for (std::uint32_t y = 0; y < height; ++y) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    float low = std::numeric_limits<float>::infinity();
                    float high = -std::numeric_limits<float>::infinity();
                    for (std::uint32_t corner = 0; corner < 4; ++corner) {
                        const std::uint32_t px = (std::min)(x * 2 + (corner & 1), previousWidth - 1);
                        const std::uint32_t py = (std::min)(y * 2 + (corner >> 1), previousHeight - 1);
                        const float parent = previous[static_cast<std::size_t>(py) * previousWidth + px];
                        low = (std::min)(low, parent);
                        high = (std::max)(high, parent);
                    }
                    const float value = valueAt(x, y);
                    asMinimum += value == low ? 1 : 0;
                    asMaximum += value == high ? 1 : 0;
                }
            }
            AbxLog("d3d12_x", "frame dump: input %s mip %u against mip %u: %llu of %llu texels equal the minimum of their 2x2 block there, %llu the maximum", path.c_str(), level,
                   level - 1, static_cast<unsigned long long>(asMinimum), static_cast<unsigned long long>(static_cast<std::uint64_t>(width) * height),
                   static_cast<unsigned long long>(asMaximum));
        }
        previous.resize(static_cast<std::size_t>(width) * height);
        std::memcpy(previous.data(), source, previous.size() * 4);
        previousWidth = width;
        previousHeight = height;
        if (level == 0) {
            const std::uint32_t step = width <= 512 ? 1u : 4u;
            const std::uint32_t outWidth = (std::max)(width / step, 1u);
            const std::uint32_t outHeight = (std::max)(height / step, 1u);
            const std::uint32_t rowBytes = (outWidth * 3 + 3) & ~3u;
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(rowBytes) * outHeight);
            for (std::uint32_t y = 0; y < outHeight; ++y) {
                for (std::uint32_t x = 0; x < outWidth; ++x) {
                    const float value = valueAt(x * step, y * step);
                    std::uint8_t* out = pixels.data() + static_cast<std::size_t>(outHeight - 1 - y) * rowBytes + static_cast<std::size_t>(x) * 3;
                    if (!std::isfinite(value)) {
                        out[2] = 255;
                    } else if (value < 0.0f) {
                        out[0] = 255;
                    } else if (value >= 1.0f) {
                        out[1] = largest > 1.0f ? static_cast<std::uint8_t>(40.0f + 215.0f * std::log(value) / std::log(largest)) : 255;
                    } else if (value > 0.0f) {
                        out[0] = out[1] = out[2] = static_cast<std::uint8_t>(40.0f + 200.0f * std::pow(value, 0.2f));
                    }
                }
            }
            if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
                const std::uint32_t imageBytes = static_cast<std::uint32_t>(pixels.size());
                const std::uint32_t fileBytes = 54 + imageBytes;
                std::uint8_t header[54] = {'B', 'M'};
                std::memcpy(header + 2, &fileBytes, 4);
                const std::uint32_t offset = 54, infoBytes = 40, planesBits = 1u | (24u << 16);
                std::memcpy(header + 10, &offset, 4);
                std::memcpy(header + 14, &infoBytes, 4);
                std::memcpy(header + 18, &outWidth, 4);
                std::memcpy(header + 22, &outHeight, 4);
                std::memcpy(header + 26, &planesBits, 4);
                std::memcpy(header + 34, &imageBytes, 4);
                std::fwrite(header, 1, sizeof(header), file);
                std::fwrite(pixels.data(), 1, pixels.size(), file);
                std::fclose(file);
            }
        }
        DestroyBuffer(device, staging);
    }
}

std::string Folder() {
    wchar_t module[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    std::string folder;
    for (DWORD index = 0; index < length; ++index) folder.push_back(static_cast<char>(module[index]));
    const std::size_t slash = folder.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : folder.substr(0, slash);
}

}

void NoteTracedTable(std::uint32_t gpu) {
    const std::lock_guard guard(g_lock);
    g_tables.insert(gpu);
}

void DumpTracedBuffers(const TitleAddressSpace* addresses, const std::string& shader, unsigned list, const std::uint32_t* words, std::size_t count) {
    constexpr UINT kSlots = 192;
    constexpr std::uint64_t kMostBytes = 2ull << 20;
    constexpr std::size_t kMostFiles = 800;
    constexpr std::uint64_t kPage = 0x10000;
    std::string wanted;
    {
        const std::lock_guard guard(g_lock);
        wanted = g_bufferShader;
    }
    if (wanted.empty() || words == nullptr || shader.find(wanted) == std::string::npos) return;
    const std::uint64_t present = Stats().presents.load();
    const std::string folder = Folder();
    for (std::size_t word = 0; word < count; ++word) {
        for (UINT slot = 0; slot < kSlots; ++slot) {
            UINT increment = 0;
            const std::uint8_t* table = DescriptorHeap::AtGpuAddress(words[word], increment);
            if (table == nullptr || increment < 16) break;
            const std::uint64_t slotGpu = static_cast<std::uint64_t>(words[word]) + static_cast<std::uint64_t>(slot) * increment;
            const std::uint8_t* at = DescriptorHeap::AtGpuAddress(slotGpu, increment);
            if (at == nullptr) break;
            std::uint32_t descriptor[4] = {};
            std::memcpy(descriptor, at, sizeof(descriptor));
            const std::uint64_t base = static_cast<std::uint64_t>(descriptor[0]) | (static_cast<std::uint64_t>(descriptor[1] & 0xFFFFu) << 32);
            if (base == 0 || (descriptor[3] >> 28) >= 8) continue;
            const std::uint32_t stride = (std::max)(descriptor[1] >> 16, 1u);
            const std::uint64_t all = static_cast<std::uint64_t>(stride) * descriptor[2];
            const std::uint64_t bytes = (std::min)(all, kMostBytes);
            if (bytes == 0) continue;
            char name[64];
            std::snprintf(name, sizeof(name), "frame%llu_buffer_%llx.bin", static_cast<unsigned long long>(present), static_cast<unsigned long long>(base));
            bool fresh = false;
            {
                const std::lock_guard guard(g_lock);
                fresh = g_bufferFiles.size() < kMostFiles && g_bufferFiles.insert({present, base}).second;
            }
            std::uint64_t written = 0;
            if (fresh) {
                std::FILE* file = std::fopen((folder + "\\" + name).c_str(), "wb");
                while (file != nullptr && written < bytes) {
                    const std::uint64_t address = base + written;
                    const std::uint64_t run = (std::min)(bytes - written, kPage - (address & (kPage - 1)));
                    const std::uint8_t* host = addresses != nullptr ? addresses->HostAt(address) : nullptr;
                    if (host == nullptr) {
                        MEMORY_BASIC_INFORMATION region{};
                        if (VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT ||
                            (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 || reinterpret_cast<std::uint64_t>(region.BaseAddress) + region.RegionSize < address + run) {
                            break;
                        }
                        host = reinterpret_cast<const std::uint8_t*>(address);
                    }
                    if (std::fwrite(host, 1, static_cast<std::size_t>(run), file) != run) break;
                    written += run;
                }
                if (file != nullptr) std::fclose(file);
            }
            AbxLog("d3d12_x", "TRACE     buffer of %s root word %zu slot %u: title 0x%llx, stride %u, %u element(s), words %08x %08x %08x %08x; %s %s (%llu of %llu byte(s) as the CPU reads them) | list %04x",
                   shader.substr(0, 19).c_str(), word, slot, static_cast<unsigned long long>(base), stride, descriptor[2], descriptor[0], descriptor[1], descriptor[2], descriptor[3],
                   fresh ? "written to" : "already in", name, static_cast<unsigned long long>(written), static_cast<unsigned long long>(all), list);
        }
    }
}

void NoteTracedDepth(Resource* target) {
    if (target == nullptr || target->IsBuffer()) return;
    const std::lock_guard guard(g_lock);
    g_depths.insert(target);
}

void NoteTracedInput(Resource* input) {
    constexpr std::size_t kMostInputs = 48;
    if (input == nullptr) return;
    const std::lock_guard guard(g_lock);
    if (g_inputs.size() < kMostInputs) g_inputs.insert(input);
}

void NoteTracedRead(Resource* read) {
    constexpr std::size_t kMostReads = 160;
    if (read == nullptr || read->IsBuffer()) return;
    const D3D12_RESOURCE_DESC& desc = read->Texture().desc;
    switch (desc.Format) {
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32_UINT: break;
    default: return;
    }
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 || desc.Width < 8 || desc.Height < 8) return;
    const std::lock_guard guard(g_lock);
    if (g_reads.size() < kMostReads) g_reads.insert(read);
}

void NoteWatchedRead(Resource* read) {
    constexpr std::size_t kMostWatched = 240;
    if (read == nullptr || read->IsBuffer()) return;
    const D3D12_RESOURCE_DESC& desc = read->Texture().desc;
    constexpr std::uint64_t kMostVolumeTexels = 16ull << 20;
    const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && desc.Width * desc.Height * desc.DepthOrArraySize <= kMostVolumeTexels;
    if ((desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D && !volume) || desc.Width < 4 || desc.Height < 4) return;
    const std::lock_guard guard(g_lock);
    if (g_watched.size() < kMostWatched) g_watched.insert(read);
}

bool WatchedShader(const std::string& shaders) {
    const std::lock_guard guard(g_lock);
    return !g_bufferShader.empty() && shaders.find(g_bufferShader) != std::string::npos;
}

void NoteTracedTarget(Resource* target) {
    if (target == nullptr) return;
    const std::lock_guard guard(g_lock);
    g_targets.insert(target);
}

void PollFrameTraceRequest() {
    static std::atomic<ULONGLONG> next{0};
    const ULONGLONG now = GetTickCount64();
    ULONGLONG due = next.load(std::memory_order_relaxed);
    if (now < due || !next.compare_exchange_strong(due, now + 500)) return;
    static const std::string fillRequest = Folder() + "\\anybox_fill_floats";
    if (std::FILE* file = std::fopen(fillRequest.c_str(), "rb")) {
        char text[96] = {};
        const std::size_t length = std::fread(text, 1, sizeof(text) - 1, file);
        std::fclose(file);
        text[length] = 0;
        DeleteFileA(fillRequest.c_str());
        unsigned width = 0;
        unsigned height = 0;
        float value = 0.0f;
        const bool on = std::sscanf(text, "%u %u %f", &width, &height, &value) == 3 && width != 0 && height != 0;
        if (TitleTextures* const textures = TitleTextures::Active()) textures->FillFloats(width, height, value, on);
    }
    static const std::string request = Folder() + "\\anybox_trace_frame";
    std::string shader;
    if (std::FILE* file = std::fopen(request.c_str(), "rb")) {
        char text[32] = {};
        const std::size_t length = std::fread(text, 1, sizeof(text) - 1, file);
        std::fclose(file);
        for (std::size_t index = 0; index < length; ++index) {
            const char letter = text[index];
            if ((letter >= '0' && letter <= '9') || (letter >= 'a' && letter <= 'f')) shader.push_back(letter);
            else break;
        }
    }
    if (!DeleteFileA(request.c_str())) return;
    const std::uint64_t first = Stats().presents.load() + 1;
    {
        const std::lock_guard guard(g_lock);
        g_bufferShader = shader.size() >= 8 ? shader : std::string();
        g_bufferFiles.clear();
    }
    if (shader.size() >= 8) {
        AbxLog("d3d12_x", "frame trace: the request file names shader %s; buffers in the first 192 slots of the root tables of its traced draws and dispatches are written as "
                          "frame<present>_buffer_<title address>.bin (up to 2 MiB each), and the 2D textures in those slots as frame<present>_watch<n>_<image>.bmp",
               shader.c_str());
    }
    TraceFrames(first, 2);
    AbxLog("d3d12_x", "frame trace requested through the file %s (now deleted): presents %llu and %llu are traced (TRACE lines) and the textures they render to are written as "
                      "frame<present>_*.bmp beside the executable",
           request.c_str(), static_cast<unsigned long long>(first), static_cast<unsigned long long>(first + 1));
}

void DumpTracedFrame(const VulkanDevice& device, Resource* presented, std::uint64_t frame) {
    std::set<Resource*> targets;
    std::set<std::uint32_t> tables;
    std::set<Resource*> depths;
    std::set<Resource*> inputs;
    std::set<Resource*> reads;
    std::set<Resource*> watched;
    {
        const std::lock_guard guard(g_lock);
        targets.swap(g_targets);
        depths.swap(g_depths);
        inputs.swap(g_inputs);
        reads.swap(g_reads);
        watched.swap(g_watched);
        tables.swap(g_tables);
    }
    for (const std::uint32_t gpu : tables) {
        UINT increment = 0;
        const std::uint8_t* table = DescriptorHeap::AtGpuAddress(gpu, increment);
        if (table == nullptr) continue;
        std::uint32_t slots[16] = {};
        std::memcpy(slots, table, sizeof(slots));
        AbxLog("d3d12_x", "TRACE     table 0x%x at present: %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x", gpu, slots[0], slots[1], slots[2], slots[3],
               slots[4], slots[5], slots[6], slots[7], slots[8], slots[9], slots[10], slots[11], slots[12], slots[13], slots[14], slots[15]);
    }
    const std::string folder = Folder();
    char name[160];
    std::snprintf(name, sizeof(name), "%s\\frame%llu_present.bmp", folder.c_str(), static_cast<unsigned long long>(frame));
    Dump(device, presented, name);
    int index = 0;
    for (Resource* target : targets) {
        std::snprintf(name, sizeof(name), "%s\\frame%llu_target%d_%p.bmp", folder.c_str(), static_cast<unsigned long long>(frame), index++,
                      static_cast<void*>(target->Texture().image));
        Dump(device, target, name);
    }
    index = 0;
    for (Resource* target : depths) {
        std::snprintf(name, sizeof(name), "%s\\frame%llu_depth%d_%p.bmp", folder.c_str(), static_cast<unsigned long long>(frame), index++,
                      static_cast<void*>(target->Texture().image));
        Dump(device, target, name, true);
    }
    index = 0;
    for (Resource* input : inputs) {
        TitleTextures* const textures = TitleTextures::Active();
        const std::string kept = textures != nullptr ? textures->Describe(input) : std::string("on a device without title textures");
        const int number = index++;
        const bool bound = DescriptorWriter::WhileBound(input, [&] {
            char inputName[160];
            std::snprintf(inputName, sizeof(inputName), "%s\\frame%llu_input%d_%p.bmp", folder.c_str(), static_cast<unsigned long long>(frame), number,
                          static_cast<void*>(input->Texture().image));
            DumpFloatLevels(device, input, inputName, kept);
        });
        if (!bound) AbxLog("d3d12_x", "frame dump: input %d of present %llu has no view left (released before the present) and is not read", number, static_cast<unsigned long long>(frame));
    }
    index = 0;
    for (Resource* read : reads) {
        const int number = index++;
        if (targets.count(read) != 0) continue;
        const bool bound = DescriptorWriter::WhileBound(read, [&] {
            char readName[160];
            std::snprintf(readName, sizeof(readName), "%s\\frame%llu_read%d_%p.bmp", folder.c_str(), static_cast<unsigned long long>(frame), number,
                          static_cast<void*>(read->Texture().image));
            AbxLog("d3d12_x", "frame dump: read %d is a texture in a traced root table, at title 0x%llx, flags 0x%x", number, static_cast<unsigned long long>(read->TextureAddress()),
                   static_cast<unsigned>(read->Texture().desc.Flags));
            Dump(device, read, readName);
        });
        if (!bound) AbxLog("d3d12_x", "frame dump: read %d of present %llu has no view left (released before the present) and is not read", number, static_cast<unsigned long long>(frame));
    }
    index = 0;
    for (Resource* read : watched) {
        const int number = index++;
        const bool bound = DescriptorWriter::WhileBound(read, [&] {
            char readName[160];
            std::snprintf(readName, sizeof(readName), "%s\\frame%llu_watch%d_%p.bmp", folder.c_str(), static_cast<unsigned long long>(frame), number,
                          static_cast<void*>(read->Texture().image));
            const D3D12_RESOURCE_DESC& desc = read->Texture().desc;
            AbxLog("d3d12_x", "frame dump: watch %d is a texture in a root table of the watched shader, at title 0x%llx, format %d, %u layer(s), %u mips, flags 0x%x", number,
                   static_cast<unsigned long long>(read->TextureAddress()), static_cast<int>(desc.Format), static_cast<unsigned>(desc.DepthOrArraySize),
                   static_cast<unsigned>(desc.MipLevels), static_cast<unsigned>(desc.Flags));
            Dump(device, read, readName, (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0);
        });
        if (!bound) AbxLog("d3d12_x", "frame dump: watch %d of present %llu has no view left (released before the present) and is not read", number, static_cast<unsigned long long>(frame));
    }
    IndirectClamp::ReportTraced(frame);
}

}
