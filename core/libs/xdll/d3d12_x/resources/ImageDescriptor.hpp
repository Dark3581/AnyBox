#pragma once

#include "TitleLayout.hpp"
#include "VulkanLoader.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace abx::d3d12x {

constexpr std::uint32_t kImageTypeBuffer = 0;
constexpr std::uint32_t kImageType1D = 8;
constexpr std::uint32_t kImageType2D = 9;
constexpr std::uint32_t kImageType3D = 10;
constexpr std::uint32_t kImageTypeCube = 11;
constexpr std::uint32_t kImageType1DArray = 12;
constexpr std::uint32_t kImageType2DArray = 13;
constexpr std::uint32_t kImageType2DMsaa = 14;
constexpr std::uint32_t kImageType2DMsaaArray = 15;

using ImageDescriptorWords = std::array<std::uint32_t, 8>;

struct ImageDescriptorFields {
    std::uint64_t base = 0;
    std::uint32_t format = 0;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::uint32_t depth = 1;
    std::array<std::uint32_t, 4> select{4, 5, 6, 7};
    std::uint32_t baseLevel = 0;
    std::uint32_t lastLevel = 0;
    std::uint32_t type = kImageType2D;
    std::uint32_t baseArray = 0;
    std::uint32_t maxMip = 0;
};

struct ImageFormatInfo {
    std::uint32_t code = 0;
    ImageComponent component = ImageComponent::Float;
};

std::optional<ImageFormatInfo> ImageFormatOf(VkFormat format);
std::optional<std::uint32_t> ComponentSelect(VkComponentSwizzle swizzle, std::uint32_t identity);
std::optional<ImageDescriptorWords> EncodeImageDescriptor(const ImageDescriptorFields& fields);
ImageDescriptorFields DecodeImageDescriptor(const ImageDescriptorWords& words);

}
