#include "SamplerDescriptor.hpp"

#include <algorithm>
#include <cmath>

namespace abx::d3d12x {

namespace {

constexpr std::uint32_t kBorderIndexLimit = 1u << 12;
constexpr std::uint32_t kBorderColorFromPointer = 3;

void Put(SamplerDescriptorWords& words, std::uint32_t low, std::uint32_t bits, std::uint32_t value) {
    for (std::uint32_t bit = 0; bit < bits; ++bit) {
        if (((value >> bit) & 1u) == 0) continue;
        const std::uint32_t at = low + bit;
        words[at / 32] |= 1u << (at % 32);
    }
}

std::optional<std::uint32_t> Clamp(D3D12_TEXTURE_ADDRESS_MODE mode) {
    switch (mode) {
    case D3D12_TEXTURE_ADDRESS_MODE_WRAP: return 0u;
    case D3D12_TEXTURE_ADDRESS_MODE_MIRROR: return 1u;
    case D3D12_TEXTURE_ADDRESS_MODE_CLAMP: return 2u;
    case D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE: return 3u;
    case D3D12_TEXTURE_ADDRESS_MODE_BORDER: return 6u;
    default: return std::nullopt;
    }
}

std::uint32_t UnsignedLod(float lod) {
    const float clamped = std::isfinite(lod) ? std::clamp(lod, 0.0f, 4095.0f / 256.0f) : 4095.0f / 256.0f;
    return static_cast<std::uint32_t>(clamped * 256.0f) & 0xFFFu;
}

std::uint32_t SignedBias(float bias) {
    const float clamped = std::clamp(bias, -32.0f, 8191.0f / 256.0f);
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(clamped * 256.0f))) & 0x3FFFu;
}

std::uint32_t AnisoRatio(UINT maxAnisotropy) {
    std::uint32_t ratio = 0;
    while (ratio < 4 && (2u << ratio) <= maxAnisotropy) ++ratio;
    return ratio;
}

}

std::optional<SamplerDescriptorWords> EncodeSamplerDescriptor(const D3D12_SAMPLER_DESC& desc, std::uint32_t borderIndex) {
    const std::optional<std::uint32_t> x = Clamp(desc.AddressU);
    const std::optional<std::uint32_t> y = Clamp(desc.AddressV);
    const std::optional<std::uint32_t> z = Clamp(desc.AddressW);
    if (!x || !y || !z || borderIndex >= kBorderIndexLimit) return std::nullopt;
    const UINT filter = static_cast<UINT>(desc.Filter);
    const bool anisotropic = (filter & D3D12_ANISOTROPIC_FILTERING_BIT) != 0;
    const std::uint32_t reduction = (filter >> D3D12_FILTER_REDUCTION_TYPE_SHIFT) & D3D12_FILTER_REDUCTION_TYPE_MASK;
    const bool comparison = reduction == D3D12_FILTER_REDUCTION_TYPE_COMPARISON;
    const std::uint32_t mag = (filter >> D3D12_MAG_FILTER_SHIFT) & D3D12_FILTER_TYPE_MASK;
    const std::uint32_t min = (filter >> D3D12_MIN_FILTER_SHIFT) & D3D12_FILTER_TYPE_MASK;
    const std::uint32_t mip = (filter >> D3D12_MIP_FILTER_SHIFT) & D3D12_FILTER_TYPE_MASK;
    SamplerDescriptorWords words{};
    Put(words, 0, 3, *x);
    Put(words, 3, 3, *y);
    Put(words, 6, 3, *z);
    Put(words, 9, 3, anisotropic ? AnisoRatio(desc.MaxAnisotropy) : 0u);
    Put(words, 12, 3, comparison && desc.ComparisonFunc >= D3D12_COMPARISON_FUNC_NEVER ? static_cast<std::uint32_t>(desc.ComparisonFunc) - 1u : 0u);
    Put(words, 29, 2, reduction == D3D12_FILTER_REDUCTION_TYPE_MINIMUM ? 1u : reduction == D3D12_FILTER_REDUCTION_TYPE_MAXIMUM ? 2u : 0u);
    Put(words, 32, 12, UnsignedLod(desc.MinLOD));
    Put(words, 44, 12, UnsignedLod(desc.MaxLOD));
    const std::uint32_t bias = SignedBias(desc.MipLODBias);
    Put(words, 64, 12, bias & 0xFFFu);
    Put(words, 76, 2, bias >> 12);
    Put(words, 84, 2, anisotropic ? 3u : mag);
    Put(words, 86, 2, anisotropic ? 3u : min);
    Put(words, 90, 2, mip + 1);
    Put(words, 96, 12, borderIndex);
    Put(words, 126, 2, kBorderColorFromPointer);
    return words;
}

}
