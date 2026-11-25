#pragma once

#include "ViewDesc.hpp"

#include <windows.h>
#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace abx::d3d12x {

constexpr std::uint32_t kRootSignatureWordLimit = D3D12_MAX_ROOT_COST;

struct RootParameterLayout {
    D3D12_ROOT_PARAMETER_TYPE type = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL;
    UINT shaderRegister = 0;
    UINT registerSpace = 0;
    UINT constants = 0;
    D3D12_ROOT_DESCRIPTOR_FLAGS descriptorFlags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    std::vector<D3D12_DESCRIPTOR_RANGE1> ranges;
    std::uint32_t words = 0;
    std::uint32_t firstWord = 0;
};

struct StaticSamplerLayout {
    D3D12_STATIC_SAMPLER_DESC desc{};
    Sampler sampler;
};

struct RootSignatureLayout {
    D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    std::vector<RootParameterLayout> parameters;
    std::vector<StaticSamplerLayout> staticSamplers;
    std::uint32_t words = 0;
    std::uint32_t heapIndexing = 0;
};

std::optional<D3D12_SAMPLER_DESC> StaticSamplerDesc(const D3D12_STATIC_SAMPLER_DESC& desc, std::string& error);
std::optional<RootSignatureLayout> BuildRootSignatureLayout(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc, std::string& error);
constexpr std::uint32_t kHeapIndexingFlags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

void RememberHeapIndexing(const void* blob, std::size_t bytes, std::uint32_t flags);
std::uint32_t HeapIndexingOf(const void* blob, std::size_t bytes);

std::optional<RootSignatureLayout> DeserializeRootSignature(const void* blob, std::size_t bytes, std::string& error);

}
