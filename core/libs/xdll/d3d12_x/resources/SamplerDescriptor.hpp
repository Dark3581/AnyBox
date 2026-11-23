#pragma once

#include <windows.h>
#include <d3d12.h>

#include <array>
#include <cstdint>
#include <optional>

namespace abx::d3d12x {

using SamplerDescriptorWords = std::array<std::uint32_t, 4>;

std::optional<SamplerDescriptorWords> EncodeSamplerDescriptor(const D3D12_SAMPLER_DESC& desc, std::uint32_t borderIndex);

}
