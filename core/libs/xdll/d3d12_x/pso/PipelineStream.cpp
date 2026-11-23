#include "PipelineStream.hpp"

#include <climits>
#include <cstdint>
#include <cstring>

namespace abx::d3d12x {

namespace {

constexpr std::size_t kTypeBytes = sizeof(std::uint32_t);
constexpr std::size_t kSubobjectAlignment = alignof(void*);

constexpr std::size_t AlignUp(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

struct Layout {
    std::size_t offset = 0;
    std::size_t size = 0;
    std::size_t total = 0;
};

template <typename Inner>
constexpr Layout LayoutOf() {
    const std::size_t offset = AlignUp(kTypeBytes, alignof(Inner));
    return {offset, sizeof(Inner), AlignUp(offset + sizeof(Inner), kSubobjectAlignment)};
}

std::optional<Layout> LayoutFor(std::uint32_t type) {
    switch (type) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: return LayoutOf<ID3D12RootSignature*>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: return LayoutOf<D3D12_SHADER_BYTECODE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: return LayoutOf<D3D12_STREAM_OUTPUT_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: return LayoutOf<D3D12_BLEND_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: return LayoutOf<UINT>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: return LayoutOf<D3D12_RASTERIZER_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: return LayoutOf<D3D12_DEPTH_STENCIL_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: return LayoutOf<D3D12_INPUT_LAYOUT_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: return LayoutOf<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: return LayoutOf<D3D12_PRIMITIVE_TOPOLOGY_TYPE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: return LayoutOf<D3D12_RT_FORMAT_ARRAY>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: return LayoutOf<DXGI_FORMAT>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: return LayoutOf<DXGI_SAMPLE_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: return LayoutOf<UINT>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: return LayoutOf<D3D12_CACHED_PIPELINE_STATE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: return LayoutOf<D3D12_PIPELINE_STATE_FLAGS>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: return LayoutOf<D3D12_DEPTH_STENCIL_DESC1>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: return LayoutOf<D3D12_VIEW_INSTANCING_DESC>();
    default: return std::nullopt;
    }
}

void SetDefaults(D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
    for (D3D12_RENDER_TARGET_BLEND_DESC& target : desc.BlendState.RenderTarget) {
        target.SrcBlend = D3D12_BLEND_ONE;
        target.DestBlend = D3D12_BLEND_ZERO;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlendAlpha = D3D12_BLEND_ZERO;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    desc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    for (D3D12_DEPTH_STENCILOP_DESC* face : {&desc.DepthStencilState.FrontFace, &desc.DepthStencilState.BackFace}) {
        face->StencilFailOp = D3D12_STENCIL_OP_KEEP;
        face->StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        face->StencilPassOp = D3D12_STENCIL_OP_KEEP;
        face->StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    }
    desc.SampleDesc.Count = 1;
}

template <typename Inner>
Inner Read(const std::uint8_t* at) {
    Inner value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

}

std::optional<ParsedPipelineStream> ParsePipelineStream(const void* stream, std::size_t bytes, std::string& error) {
    if (stream == nullptr || bytes == 0) {
        error = "an empty pipeline state stream";
        return std::nullopt;
    }
    ParsedPipelineStream parsed;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc = parsed.graphics;
    SetDefaults(desc);
    const auto* base = static_cast<const std::uint8_t*>(stream);
    std::size_t at = 0;
    while (at < bytes) {
        if (bytes - at < kTypeBytes) {
            error = "a pipeline state stream ends inside a subobject type";
            return std::nullopt;
        }
        const std::uint32_t type = Read<std::uint32_t>(base + at);
        const std::optional<Layout> layout = LayoutFor(type);
        if (!layout) {
            error = "pipeline state subobject type " + std::to_string(type) + " is not a known desktop type (after " + parsed.types + ")";
            return std::nullopt;
        }
        if (bytes - at < layout->offset + layout->size) {
            error = "pipeline state subobject type " + std::to_string(type) + " runs past the end of the stream";
            return std::nullopt;
        }
        parsed.types += (parsed.types.empty() ? "" : ",") + std::to_string(type);
        const std::uint8_t* inner = base + at + layout->offset;
        switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: desc.pRootSignature = Read<ID3D12RootSignature*>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: desc.VS = Read<D3D12_SHADER_BYTECODE>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: desc.PS = Read<D3D12_SHADER_BYTECODE>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: desc.DS = Read<D3D12_SHADER_BYTECODE>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: desc.HS = Read<D3D12_SHADER_BYTECODE>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: desc.GS = Read<D3D12_SHADER_BYTECODE>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: parsed.compute = Read<D3D12_SHADER_BYTECODE>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: parsed.amplification = Read<D3D12_SHADER_BYTECODE>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: parsed.mesh = Read<D3D12_SHADER_BYTECODE>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: desc.StreamOutput = Read<D3D12_STREAM_OUTPUT_DESC>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: desc.BlendState = Read<D3D12_BLEND_DESC>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: desc.SampleMask = Read<UINT>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: desc.RasterizerState = Read<D3D12_RASTERIZER_DESC>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: desc.DepthStencilState = Read<D3D12_DEPTH_STENCIL_DESC>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: {
            const D3D12_DEPTH_STENCIL_DESC1 depth = Read<D3D12_DEPTH_STENCIL_DESC1>(inner);
            if (depth.DepthBoundsTestEnable) {
                error = "depth bounds tests in a pipeline state stream are not supported";
                return std::nullopt;
            }
            std::memcpy(&desc.DepthStencilState, &depth, sizeof(desc.DepthStencilState));
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: desc.InputLayout = Read<D3D12_INPUT_LAYOUT_DESC>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: desc.IBStripCutValue = Read<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: desc.PrimitiveTopologyType = Read<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: {
            const D3D12_RT_FORMAT_ARRAY formats = Read<D3D12_RT_FORMAT_ARRAY>(inner);
            desc.NumRenderTargets = formats.NumRenderTargets;
            std::memcpy(desc.RTVFormats, formats.RTFormats, sizeof(desc.RTVFormats));
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: desc.DSVFormat = Read<DXGI_FORMAT>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: desc.SampleDesc = Read<DXGI_SAMPLE_DESC>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: desc.NodeMask = Read<UINT>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: desc.CachedPSO = Read<D3D12_CACHED_PIPELINE_STATE>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: desc.Flags = Read<D3D12_PIPELINE_STATE_FLAGS>(inner); break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
            const D3D12_VIEW_INSTANCING_DESC instancing = Read<D3D12_VIEW_INSTANCING_DESC>(inner);
            if (instancing.ViewInstanceCount > 1) {
                error = "view instancing in a pipeline state stream is not supported";
                return std::nullopt;
            }
            break;
        }
        default: break;
        }
        at += layout->total;
    }
    return parsed;
}

}
