#pragma once

#include <windows.h>
#include <d3d12.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace abx::d3d12x {

using BufferDescriptorWords = std::array<std::uint32_t, 4>;

enum class BufferBounds : std::uint32_t { StructuredWithOffset = 0, Structured = 1, Disabled = 2, Raw = 3 };

enum DescriptorSelect : std::uint32_t { SelectZero = 0, SelectOne = 1, SelectX = 4, SelectY = 5, SelectZ = 6, SelectW = 7 };

struct BufferDescriptorFields {
    std::uint64_t base = 0;
    std::uint32_t stride = 0;
    std::uint32_t records = 0;
    std::uint32_t format = 0;
    std::array<std::uint32_t, 4> select{SelectX, SelectY, SelectZ, SelectW};
    BufferBounds bounds = BufferBounds::Raw;
};

struct TypedBufferFormat {
    std::uint32_t code = 0;
    std::uint32_t bytes = 0;
    std::uint32_t components = 0;
};

constexpr std::uint64_t kDescriptorAddressLimit = 1ull << 48;
constexpr std::uint32_t kDescriptorStrideLimit = (1u << 14) - 1;

std::optional<BufferDescriptorWords> EncodeBufferDescriptor(const BufferDescriptorFields& fields, std::string& error);
BufferDescriptorFields DecodeBufferDescriptor(const BufferDescriptorWords& words);
std::optional<TypedBufferFormat> TypedBufferFormatOf(DXGI_FORMAT format);

std::optional<BufferDescriptorWords> ConstantBufferDescriptor(const D3D12_CONSTANT_BUFFER_VIEW_DESC& desc, std::string& error);
std::optional<BufferDescriptorWords> ShaderResourceBufferDescriptor(UINT64 resourceAddress, UINT64 resourceBytes, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, std::string& error);
std::optional<BufferDescriptorWords> UnorderedAccessBufferDescriptor(UINT64 resourceAddress, UINT64 resourceBytes, const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc, bool counter,
                                                                    std::string& error);

}
