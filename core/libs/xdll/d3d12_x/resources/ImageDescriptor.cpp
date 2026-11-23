#include "ImageDescriptor.hpp"

namespace abx::d3d12x {

namespace {

constexpr std::uint64_t kBaseLimit = 1ull << 40;

bool Fits(std::uint64_t value, std::uint32_t bits) {
    return bits >= 64 || value < (1ull << bits);
}

void Put(ImageDescriptorWords& words, std::uint32_t low, std::uint32_t bits, std::uint64_t value) {
    for (std::uint32_t bit = 0; bit < bits; ++bit) {
        if (((value >> bit) & 1u) == 0) continue;
        const std::uint32_t at = low + bit;
        words[at / 32] |= 1u << (at % 32);
    }
}

std::uint64_t Get(const ImageDescriptorWords& words, std::uint32_t low, std::uint32_t bits) {
    std::uint64_t value = 0;
    for (std::uint32_t bit = 0; bit < bits; ++bit) {
        const std::uint32_t at = low + bit;
        value |= static_cast<std::uint64_t>((words[at / 32] >> (at % 32)) & 1u) << bit;
    }
    return value;
}

ImageFormatInfo Float(std::uint32_t code) {
    return {code, ImageComponent::Float};
}

ImageFormatInfo Uint(std::uint32_t code) {
    return {code, ImageComponent::Uint};
}

ImageFormatInfo Sint(std::uint32_t code) {
    return {code, ImageComponent::Sint};
}

}

std::optional<ImageFormatInfo> ImageFormatOf(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UNORM: return Float(1);
    case VK_FORMAT_R8_SNORM: return Float(2);
    case VK_FORMAT_R8_UINT: return Uint(5);
    case VK_FORMAT_R8_SINT: return Sint(6);
    case VK_FORMAT_R16_UNORM: return Float(7);
    case VK_FORMAT_R16_SNORM: return Float(8);
    case VK_FORMAT_R16_UINT: return Uint(11);
    case VK_FORMAT_R16_SINT: return Sint(12);
    case VK_FORMAT_R16_SFLOAT: return Float(13);
    case VK_FORMAT_R8G8_UNORM: return Float(14);
    case VK_FORMAT_R8G8_SNORM: return Float(15);
    case VK_FORMAT_R8G8_UINT: return Uint(18);
    case VK_FORMAT_R8G8_SINT: return Sint(19);
    case VK_FORMAT_R32_UINT: return Uint(20);
    case VK_FORMAT_R32_SINT: return Sint(21);
    case VK_FORMAT_R32_SFLOAT: return Float(22);
    case VK_FORMAT_D32_SFLOAT: return Float(22);
    case VK_FORMAT_R16G16_UNORM: return Float(23);
    case VK_FORMAT_R16G16_SNORM: return Float(24);
    case VK_FORMAT_R16G16_UINT: return Uint(27);
    case VK_FORMAT_R16G16_SINT: return Sint(28);
    case VK_FORMAT_R16G16_SFLOAT: return Float(29);
    case VK_FORMAT_R8G8B8A8_UNORM: return Float(56);
    case VK_FORMAT_B8G8R8A8_UNORM: return Float(56);
    case VK_FORMAT_R8G8B8A8_SNORM: return Float(57);
    case VK_FORMAT_R8G8B8A8_UINT: return Uint(60);
    case VK_FORMAT_R8G8B8A8_SINT: return Sint(61);
    case VK_FORMAT_R32G32_UINT: return Uint(62);
    case VK_FORMAT_R32G32_SINT: return Sint(63);
    case VK_FORMAT_R32G32_SFLOAT: return Float(64);
    case VK_FORMAT_R16G16B16A16_UNORM: return Float(65);
    case VK_FORMAT_R16G16B16A16_SNORM: return Float(66);
    case VK_FORMAT_R16G16B16A16_UINT: return Uint(69);
    case VK_FORMAT_R16G16B16A16_SINT: return Sint(70);
    case VK_FORMAT_R16G16B16A16_SFLOAT: return Float(71);
    case VK_FORMAT_R32G32B32_UINT: return Uint(72);
    case VK_FORMAT_R32G32B32_SINT: return Sint(73);
    case VK_FORMAT_R32G32B32_SFLOAT: return Float(74);
    case VK_FORMAT_R32G32B32A32_UINT: return Uint(75);
    case VK_FORMAT_R32G32B32A32_SINT: return Sint(76);
    case VK_FORMAT_R32G32B32A32_SFLOAT: return Float(77);
    case VK_FORMAT_R8_SRGB: return Float(128);
    case VK_FORMAT_R8G8_SRGB: return Float(129);
    case VK_FORMAT_R8G8B8A8_SRGB: return Float(130);
    case VK_FORMAT_B8G8R8A8_SRGB: return Float(130);
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return Float(132);
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return Float(169);
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return Float(170);
    case VK_FORMAT_BC2_UNORM_BLOCK: return Float(171);
    case VK_FORMAT_BC2_SRGB_BLOCK: return Float(172);
    case VK_FORMAT_BC3_UNORM_BLOCK: return Float(173);
    case VK_FORMAT_BC3_SRGB_BLOCK: return Float(174);
    case VK_FORMAT_BC4_UNORM_BLOCK: return Float(175);
    case VK_FORMAT_BC4_SNORM_BLOCK: return Float(176);
    case VK_FORMAT_BC5_UNORM_BLOCK: return Float(177);
    case VK_FORMAT_BC5_SNORM_BLOCK: return Float(178);
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: return Float(179);
    case VK_FORMAT_BC6H_SFLOAT_BLOCK: return Float(180);
    case VK_FORMAT_BC7_UNORM_BLOCK: return Float(181);
    case VK_FORMAT_BC7_SRGB_BLOCK: return Float(182);
    default: return std::nullopt;
    }
}

std::optional<std::uint32_t> ComponentSelect(VkComponentSwizzle swizzle, std::uint32_t identity) {
    switch (swizzle) {
    case VK_COMPONENT_SWIZZLE_IDENTITY: return identity;
    case VK_COMPONENT_SWIZZLE_ZERO: return 0u;
    case VK_COMPONENT_SWIZZLE_ONE: return 1u;
    case VK_COMPONENT_SWIZZLE_R: return 4u;
    case VK_COMPONENT_SWIZZLE_G: return 5u;
    case VK_COMPONENT_SWIZZLE_B: return 6u;
    case VK_COMPONENT_SWIZZLE_A: return 7u;
    default: return std::nullopt;
    }
}

std::optional<ImageDescriptorWords> EncodeImageDescriptor(const ImageDescriptorFields& fields) {
    if (fields.base >= kBaseLimit || !Fits(fields.format, 9) || fields.width == 0 || fields.height == 0 || fields.depth == 0 || !Fits(fields.width - 1, 14) ||
        !Fits(fields.height - 1, 14) || !Fits(fields.depth - 1, 13) || !Fits(fields.baseLevel, 4) || !Fits(fields.lastLevel, 4) || !Fits(fields.type, 4) ||
        !Fits(fields.baseArray, 13) || !Fits(fields.maxMip, 4)) {
        return std::nullopt;
    }
    ImageDescriptorWords words{};
    Put(words, 0, 40, fields.base);
    Put(words, 52, 9, fields.format);
    Put(words, 62, 16, fields.width - 1);
    Put(words, 78, 16, fields.height - 1);
    Put(words, 95, 1, 1);
    for (std::uint32_t component = 0; component < 4; ++component) {
        if (!Fits(fields.select[component], 3)) return std::nullopt;
        Put(words, 96 + 3 * component, 3, fields.select[component]);
    }
    Put(words, 108, 4, fields.baseLevel);
    Put(words, 112, 4, fields.lastLevel);
    Put(words, 124, 4, fields.type);
    Put(words, 128, 13, fields.depth - 1);
    Put(words, 144, 13, fields.baseArray);
    Put(words, 164, 4, fields.maxMip);
    return words;
}

ImageDescriptorFields DecodeImageDescriptor(const ImageDescriptorWords& words) {
    ImageDescriptorFields fields;
    fields.base = Get(words, 0, 40);
    fields.format = static_cast<std::uint32_t>(Get(words, 52, 9));
    fields.width = static_cast<std::uint32_t>(Get(words, 62, 16)) + 1;
    fields.height = static_cast<std::uint32_t>(Get(words, 78, 16)) + 1;
    for (std::uint32_t component = 0; component < 4; ++component) {
        fields.select[component] = static_cast<std::uint32_t>(Get(words, 96 + 3 * component, 3));
    }
    fields.baseLevel = static_cast<std::uint32_t>(Get(words, 108, 4));
    fields.lastLevel = static_cast<std::uint32_t>(Get(words, 112, 4));
    fields.type = static_cast<std::uint32_t>(Get(words, 124, 4));
    fields.depth = static_cast<std::uint32_t>(Get(words, 128, 13)) + 1;
    fields.baseArray = static_cast<std::uint32_t>(Get(words, 144, 13));
    fields.maxMip = static_cast<std::uint32_t>(Get(words, 164, 4));
    return fields;
}

}
