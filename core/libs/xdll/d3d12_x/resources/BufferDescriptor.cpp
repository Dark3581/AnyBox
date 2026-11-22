#include "BufferDescriptor.hpp"

namespace abx::d3d12x {

namespace {

constexpr UINT kAllComponents = 0xFFFFFFFFu;

std::uint32_t DefaultSelect(std::uint32_t component) {
    return component == 3 ? SelectOne : SelectZero;
}

std::array<std::uint32_t, 4> TypedSelect(std::uint32_t components) {
    std::array<std::uint32_t, 4> select{};
    for (std::uint32_t component = 0; component < 4; ++component) {
        select[component] = component < components ? SelectX + component : DefaultSelect(component);
    }
    return select;
}

std::optional<std::array<std::uint32_t, 4>> MappedSelect(UINT mapping, std::uint32_t components) {
    if (mapping == kAllComponents) {
        mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    }
    if ((mapping & D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES) == 0) {
        return std::nullopt;
    }
    std::array<std::uint32_t, 4> select{};
    for (std::uint32_t component = 0; component < 4; ++component) {
        const UINT source = (mapping >> (3 * component)) & 7u;
        if (source <= 3) {
            select[component] = source < components ? SelectX + source : DefaultSelect(source);
        } else if (source == D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0) {
            select[component] = SelectZero;
        } else if (source == D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1) {
            select[component] = SelectOne;
        } else {
            return std::nullopt;
        }
    }
    return select;
}

struct ElementLayout {
    std::uint32_t bytes = 0;
    BufferDescriptorFields fields;
};

std::optional<ElementLayout> BufferLayout(DXGI_FORMAT format, UINT structureStride, bool raw, std::string& error) {
    ElementLayout layout;
    if (raw) {
        if (format != DXGI_FORMAT_R32_TYPELESS || structureStride != 0) {
            error = "a raw buffer view needs R32_TYPELESS and no structure stride";
            return std::nullopt;
        }
        layout.bytes = 4;
        layout.fields.bounds = BufferBounds::Raw;
        layout.fields.format = TypedBufferFormatOf(DXGI_FORMAT_R32_UINT)->code;
        return layout;
    }
    if (structureStride != 0) {
        if (format != DXGI_FORMAT_UNKNOWN) {
            error = "a structured buffer view must have format UNKNOWN";
            return std::nullopt;
        }
        if (structureStride % 4 != 0 || structureStride > kDescriptorStrideLimit) {
            error = "structure stride " + std::to_string(structureStride) + " is not a multiple of 4 up to " + std::to_string(kDescriptorStrideLimit);
            return std::nullopt;
        }
        layout.bytes = structureStride;
        layout.fields.stride = structureStride;
        layout.fields.bounds = BufferBounds::StructuredWithOffset;
        layout.fields.format = TypedBufferFormatOf(DXGI_FORMAT_R32_UINT)->code;
        return layout;
    }
    const std::optional<TypedBufferFormat> typed = TypedBufferFormatOf(format);
    if (!typed) {
        error = "buffer view format " + std::to_string(static_cast<int>(format)) + " has no typed buffer encoding";
        return std::nullopt;
    }
    layout.bytes = typed->bytes;
    layout.fields.stride = typed->bytes;
    layout.fields.format = typed->code;
    layout.fields.select = TypedSelect(typed->components);
    layout.fields.bounds = BufferBounds::Structured;
    return layout;
}

std::optional<BufferDescriptorWords> ElementView(UINT64 resourceAddress, UINT64 resourceBytes, UINT64 first, UINT count, ElementLayout layout, std::string& error) {
    if (count == 0) {
        error = "the buffer view has no elements";
        return std::nullopt;
    }
    const UINT64 elements = resourceBytes / layout.bytes;
    if (first > elements || count > elements - first) {
        error = "elements " + std::to_string(first) + "+" + std::to_string(count) + " run past the buffer's " + std::to_string(elements);
        return std::nullopt;
    }
    layout.fields.base = resourceAddress + first * layout.bytes;
    const UINT64 records = layout.fields.bounds == BufferBounds::Raw ? static_cast<UINT64>(count) * layout.bytes : count;
    if (records > 0xFFFFFFFFull) {
        error = "the buffer view has more records than a descriptor holds";
        return std::nullopt;
    }
    layout.fields.records = static_cast<std::uint32_t>(records);
    return EncodeBufferDescriptor(layout.fields, error);
}

}

std::optional<BufferDescriptorWords> EncodeBufferDescriptor(const BufferDescriptorFields& fields, std::string& error) {
    if (fields.base >= kDescriptorAddressLimit) {
        error = "buffer address " + std::to_string(fields.base) + " does not fit the descriptor's 48 bits";
        return std::nullopt;
    }
    if (fields.stride > kDescriptorStrideLimit || fields.format > 0x7Fu) {
        error = "the stride or format does not fit the descriptor";
        return std::nullopt;
    }
    BufferDescriptorWords words{};
    words[0] = static_cast<std::uint32_t>(fields.base);
    words[1] = static_cast<std::uint32_t>(fields.base >> 32) & 0xFFFFu;
    words[1] |= fields.stride << 16;
    words[2] = fields.records;
    for (std::uint32_t component = 0; component < 4; ++component) {
        if (fields.select[component] > 7) {
            error = "component select " + std::to_string(fields.select[component]) + " does not fit 3 bits";
            return std::nullopt;
        }
        words[3] |= fields.select[component] << (3 * component);
    }
    words[3] |= fields.format << 12;
    words[3] |= 1u << 24;
    words[3] |= static_cast<std::uint32_t>(fields.bounds) << 28;
    return words;
}

BufferDescriptorFields DecodeBufferDescriptor(const BufferDescriptorWords& words) {
    BufferDescriptorFields fields;
    fields.base = words[0] | (static_cast<std::uint64_t>(words[1] & 0xFFFFu) << 32);
    fields.stride = (words[1] >> 16) & kDescriptorStrideLimit;
    fields.records = words[2];
    for (std::uint32_t component = 0; component < 4; ++component) {
        fields.select[component] = (words[3] >> (3 * component)) & 7u;
    }
    fields.format = (words[3] >> 12) & 0x7Fu;
    fields.bounds = static_cast<BufferBounds>((words[3] >> 28) & 3u);
    return fields;
}

std::optional<TypedBufferFormat> TypedBufferFormatOf(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8_UNORM: return TypedBufferFormat{1, 1, 1};
    case DXGI_FORMAT_R8_SNORM: return TypedBufferFormat{2, 1, 1};
    case DXGI_FORMAT_R8_UINT: return TypedBufferFormat{5, 1, 1};
    case DXGI_FORMAT_R8_SINT: return TypedBufferFormat{6, 1, 1};
    case DXGI_FORMAT_R16_UNORM: return TypedBufferFormat{7, 2, 1};
    case DXGI_FORMAT_R16_SNORM: return TypedBufferFormat{8, 2, 1};
    case DXGI_FORMAT_R16_UINT: return TypedBufferFormat{11, 2, 1};
    case DXGI_FORMAT_R16_SINT: return TypedBufferFormat{12, 2, 1};
    case DXGI_FORMAT_R16_FLOAT: return TypedBufferFormat{13, 2, 1};
    case DXGI_FORMAT_R8G8_UNORM: return TypedBufferFormat{14, 2, 2};
    case DXGI_FORMAT_R8G8_SNORM: return TypedBufferFormat{15, 2, 2};
    case DXGI_FORMAT_R8G8_UINT: return TypedBufferFormat{18, 2, 2};
    case DXGI_FORMAT_R8G8_SINT: return TypedBufferFormat{19, 2, 2};
    case DXGI_FORMAT_R32_UINT: return TypedBufferFormat{20, 4, 1};
    case DXGI_FORMAT_R32_SINT: return TypedBufferFormat{21, 4, 1};
    case DXGI_FORMAT_R32_FLOAT: return TypedBufferFormat{22, 4, 1};
    case DXGI_FORMAT_R16G16_UNORM: return TypedBufferFormat{23, 4, 2};
    case DXGI_FORMAT_R16G16_SNORM: return TypedBufferFormat{24, 4, 2};
    case DXGI_FORMAT_R16G16_UINT: return TypedBufferFormat{27, 4, 2};
    case DXGI_FORMAT_R16G16_SINT: return TypedBufferFormat{28, 4, 2};
    case DXGI_FORMAT_R16G16_FLOAT: return TypedBufferFormat{29, 4, 2};
    case DXGI_FORMAT_R11G11B10_FLOAT: return TypedBufferFormat{36, 4, 3};
    case DXGI_FORMAT_R10G10B10A2_UNORM: return TypedBufferFormat{50, 4, 4};
    case DXGI_FORMAT_R10G10B10A2_UINT: return TypedBufferFormat{54, 4, 4};
    case DXGI_FORMAT_R8G8B8A8_UNORM: return TypedBufferFormat{56, 4, 4};
    case DXGI_FORMAT_R8G8B8A8_SNORM: return TypedBufferFormat{57, 4, 4};
    case DXGI_FORMAT_R8G8B8A8_UINT: return TypedBufferFormat{60, 4, 4};
    case DXGI_FORMAT_R8G8B8A8_SINT: return TypedBufferFormat{61, 4, 4};
    case DXGI_FORMAT_R32G32_UINT: return TypedBufferFormat{62, 8, 2};
    case DXGI_FORMAT_R32G32_SINT: return TypedBufferFormat{63, 8, 2};
    case DXGI_FORMAT_R32G32_FLOAT: return TypedBufferFormat{64, 8, 2};
    case DXGI_FORMAT_R16G16B16A16_UNORM: return TypedBufferFormat{65, 8, 4};
    case DXGI_FORMAT_R16G16B16A16_SNORM: return TypedBufferFormat{66, 8, 4};
    case DXGI_FORMAT_R16G16B16A16_UINT: return TypedBufferFormat{69, 8, 4};
    case DXGI_FORMAT_R16G16B16A16_SINT: return TypedBufferFormat{70, 8, 4};
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return TypedBufferFormat{71, 8, 4};
    case DXGI_FORMAT_R32G32B32_UINT: return TypedBufferFormat{72, 12, 3};
    case DXGI_FORMAT_R32G32B32_SINT: return TypedBufferFormat{73, 12, 3};
    case DXGI_FORMAT_R32G32B32_FLOAT: return TypedBufferFormat{74, 12, 3};
    case DXGI_FORMAT_R32G32B32A32_UINT: return TypedBufferFormat{75, 16, 4};
    case DXGI_FORMAT_R32G32B32A32_SINT: return TypedBufferFormat{76, 16, 4};
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return TypedBufferFormat{77, 16, 4};
    default: return std::nullopt;
    }
}

std::optional<BufferDescriptorWords> ConstantBufferDescriptor(const D3D12_CONSTANT_BUFFER_VIEW_DESC& desc, std::string& error) {
    if (desc.BufferLocation == 0 || desc.SizeInBytes == 0 || desc.SizeInBytes % 16 != 0) {
        error = "a constant buffer view needs an address and a size that is a nonzero multiple of 16 bytes";
        return std::nullopt;
    }
    BufferDescriptorFields fields;
    fields.base = desc.BufferLocation;
    fields.records = desc.SizeInBytes;
    fields.bounds = BufferBounds::Raw;
    fields.format = TypedBufferFormatOf(DXGI_FORMAT_R32_FLOAT)->code;
    return EncodeBufferDescriptor(fields, error);
}

std::optional<BufferDescriptorWords> ShaderResourceBufferDescriptor(UINT64 resourceAddress, UINT64 resourceBytes, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, std::string& error) {
    if (desc.ViewDimension != D3D12_SRV_DIMENSION_BUFFER) {
        error = "shader resource view dimension " + std::to_string(static_cast<int>(desc.ViewDimension)) + " is not a buffer";
        return std::nullopt;
    }
    const UINT flags = static_cast<UINT>(desc.Buffer.Flags);
    if ((flags & ~static_cast<UINT>(D3D12_BUFFER_SRV_FLAG_RAW)) != 0) {
        error = "buffer view flags " + std::to_string(flags) + " are unknown";
        return std::nullopt;
    }
    std::optional<ElementLayout> layout = BufferLayout(desc.Format, desc.Buffer.StructureByteStride, (flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0, error);
    if (!layout) {
        return std::nullopt;
    }
    if (layout->fields.bounds == BufferBounds::Structured) {
        const std::optional<TypedBufferFormat> typed = TypedBufferFormatOf(desc.Format);
        const std::optional<std::array<std::uint32_t, 4>> select = MappedSelect(desc.Shader4ComponentMapping, typed->components);
        if (!select) {
            error = "component mapping " + std::to_string(desc.Shader4ComponentMapping) + " is not valid";
            return std::nullopt;
        }
        layout->fields.select = *select;
    }
    return ElementView(resourceAddress, resourceBytes, desc.Buffer.FirstElement, desc.Buffer.NumElements, *layout, error);
}

std::optional<BufferDescriptorWords> UnorderedAccessBufferDescriptor(UINT64 resourceAddress, UINT64 resourceBytes, const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc, bool counter,
                                                                    std::string& error) {
    if (desc.ViewDimension != D3D12_UAV_DIMENSION_BUFFER) {
        error = "unordered access view dimension " + std::to_string(static_cast<int>(desc.ViewDimension)) + " is not a buffer";
        return std::nullopt;
    }
    if (counter || desc.Buffer.CounterOffsetInBytes != 0) {
        error = "unordered access views with a counter are not supported";
        return std::nullopt;
    }
    const UINT flags = static_cast<UINT>(desc.Buffer.Flags);
    if ((flags & ~static_cast<UINT>(D3D12_BUFFER_UAV_FLAG_RAW)) != 0) {
        error = "buffer view flags " + std::to_string(flags) + " are unknown";
        return std::nullopt;
    }
    std::optional<ElementLayout> layout = BufferLayout(desc.Format, desc.Buffer.StructureByteStride, (flags & D3D12_BUFFER_UAV_FLAG_RAW) != 0, error);
    if (!layout) {
        return std::nullopt;
    }
    return ElementView(resourceAddress, resourceBytes, desc.Buffer.FirstElement, desc.Buffer.NumElements, *layout, error);
}

}
