#pragma once

#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <optional>
#include <string>

namespace abx::d3d12x {

constexpr UINT kImageDescriptorBytes = 32;
constexpr UINT kBufferDescriptorBytes = 16;
constexpr UINT kSamplerDescriptorBytes = 16;

struct DescriptorSizes {
    std::optional<UINT> shaderResource;
    std::optional<UINT> sampler;
    std::optional<UINT> renderTarget;
    std::optional<UINT> depthStencil;
};

bool ParseDescriptorSize(const char* key, const char* text, std::optional<UINT>& size, std::string& error);
std::optional<DescriptorSizes> DescriptorSizesFromSettings(std::string& error);
std::optional<UINT> DescriptorIncrement(const DescriptorSizes& sizes, D3D12_DESCRIPTOR_HEAP_TYPE type, std::string& error);
std::optional<std::uint64_t> DescriptorHeapBytes(UINT count, UINT increment, std::string& error);

}
