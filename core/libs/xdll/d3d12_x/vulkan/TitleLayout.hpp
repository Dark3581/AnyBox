#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace abx::d3d12x {

enum class DescriptorKind { SampledImage, Sampler, StorageBuffer, StorageImage };
enum class ImageDim { None, Dim1D, Dim2D, Dim3D };
enum class ImageComponent { None, Float, Uint, Sint };
enum class ImageFormat { None, Unknown, R32ui };

struct BindingSpec {
    std::uint32_t binding = 0;
    DescriptorKind kind = DescriptorKind::SampledImage;
    ImageDim dim = ImageDim::None;
    bool arrayed = false;
    ImageComponent component = ImageComponent::None;
    ImageFormat format = ImageFormat::None;
    std::uint32_t count = 0;
};

constexpr std::uint32_t kTitlePushConstantBytes = 144;

const std::vector<BindingSpec>& TitleBindings();
const char* KindName(DescriptorKind kind);
const char* DimName(ImageDim dim);
const char* ComponentName(ImageComponent component);
const char* FormatName(ImageFormat format);
std::string TitleLayoutJson();

}
