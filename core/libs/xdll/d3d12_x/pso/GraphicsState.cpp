#include "GraphicsState.hpp"

#include <cmath>

namespace abx::d3d12x {

namespace {

constexpr UINT kStandardMultisamplePattern = 0xFFFFFFFFu;

bool IsDualSource(D3D12_BLEND blend) {
    return blend == D3D12_BLEND_SRC1_COLOR || blend == D3D12_BLEND_INV_SRC1_COLOR || blend == D3D12_BLEND_SRC1_ALPHA || blend == D3D12_BLEND_INV_SRC1_ALPHA;
}

bool SameBlend(const D3D12_RENDER_TARGET_BLEND_DESC& a, const D3D12_RENDER_TARGET_BLEND_DESC& b) {
    return a.BlendEnable == b.BlendEnable && a.SrcBlend == b.SrcBlend && a.DestBlend == b.DestBlend && a.BlendOp == b.BlendOp && a.SrcBlendAlpha == b.SrcBlendAlpha &&
           a.DestBlendAlpha == b.DestBlendAlpha && a.BlendOpAlpha == b.BlendOpAlpha && a.RenderTargetWriteMask == b.RenderTargetWriteMask;
}

std::optional<VkStencilOpState> StencilFace(const D3D12_DEPTH_STENCILOP_DESC& face, UINT8 readMask, UINT8 writeMask, std::string& error, const char* name) {
    const std::optional<VkStencilOp> fail = VulkanStencilOp(face.StencilFailOp);
    const std::optional<VkStencilOp> depthFail = VulkanStencilOp(face.StencilDepthFailOp);
    const std::optional<VkStencilOp> pass = VulkanStencilOp(face.StencilPassOp);
    const std::optional<VkCompareOp> compare = VulkanCompareOp(face.StencilFunc);
    if (!fail || !depthFail || !pass || !compare) {
        error = std::string("the ") + name + " stencil face has an unknown operation or comparison";
        return std::nullopt;
    }
    VkStencilOpState state{};
    state.failOp = *fail;
    state.passOp = *pass;
    state.depthFailOp = *depthFail;
    state.compareOp = *compare;
    state.compareMask = readMask;
    state.writeMask = writeMask;
    return state;
}

}

std::optional<VkFormat> VulkanFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case DXGI_FORMAT_R32G32B32A32_UINT: return VK_FORMAT_R32G32B32A32_UINT;
    case DXGI_FORMAT_R32G32B32A32_SINT: return VK_FORMAT_R32G32B32A32_SINT;
    case DXGI_FORMAT_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
    case DXGI_FORMAT_R32G32B32_UINT: return VK_FORMAT_R32G32B32_UINT;
    case DXGI_FORMAT_R32G32B32_SINT: return VK_FORMAT_R32G32B32_SINT;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case DXGI_FORMAT_R16G16B16A16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case DXGI_FORMAT_R16G16B16A16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case DXGI_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case DXGI_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case DXGI_FORMAT_R32G32_UINT: return VK_FORMAT_R32G32_UINT;
    case DXGI_FORMAT_R32G32_SINT: return VK_FORMAT_R32G32_SINT;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case DXGI_FORMAT_R10G10B10A2_UINT: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case DXGI_FORMAT_R11G11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case DXGI_FORMAT_R8G8B8A8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case DXGI_FORMAT_R8G8B8A8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case DXGI_FORMAT_R8G8B8A8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case DXGI_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case DXGI_FORMAT_R16G16_UNORM: return VK_FORMAT_R16G16_UNORM;
    case DXGI_FORMAT_R16G16_UINT: return VK_FORMAT_R16G16_UINT;
    case DXGI_FORMAT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
    case DXGI_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
    case DXGI_FORMAT_D32_FLOAT: return VK_FORMAT_D32_SFLOAT;
    case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case DXGI_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
    case DXGI_FORMAT_R32_SINT: return VK_FORMAT_R32_SINT;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
    case kConsoleD16UnormS8Uint: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case DXGI_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R8G8_UINT: return VK_FORMAT_R8G8_UINT;
    case DXGI_FORMAT_R8G8_SNORM: return VK_FORMAT_R8G8_SNORM;
    case DXGI_FORMAT_R8G8_SINT: return VK_FORMAT_R8G8_SINT;
    case DXGI_FORMAT_R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
    case DXGI_FORMAT_D16_UNORM: return VK_FORMAT_D16_UNORM;
    case DXGI_FORMAT_R16_UNORM: return VK_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R16_UINT: return VK_FORMAT_R16_UINT;
    case DXGI_FORMAT_R16_SNORM: return VK_FORMAT_R16_SNORM;
    case DXGI_FORMAT_R16_SINT: return VK_FORMAT_R16_SINT;
    case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
    case DXGI_FORMAT_A8_UNORM: return VK_FORMAT_A8_UNORM_KHR;
    case DXGI_FORMAT_R8_UINT: return VK_FORMAT_R8_UINT;
    case DXGI_FORMAT_R8_SNORM: return VK_FORMAT_R8_SNORM;
    case DXGI_FORMAT_R8_SINT: return VK_FORMAT_R8_SINT;
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case DXGI_FORMAT_B5G6R5_UNORM: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case DXGI_FORMAT_B5G5R5A1_UNORM: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case DXGI_FORMAT_B4G4R4A4_UNORM: return VK_FORMAT_A4R4G4B4_UNORM_PACK16;
    default: return std::nullopt;
    }
}

bool FormatHasStencil(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || format == kConsoleD16UnormS8Uint;
}

std::optional<VkBlendFactor> VulkanBlendFactor(D3D12_BLEND blend) {
    switch (blend) {
    case D3D12_BLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
    case D3D12_BLEND_ONE: return VK_BLEND_FACTOR_ONE;
    case D3D12_BLEND_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case D3D12_BLEND_INV_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case D3D12_BLEND_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case D3D12_BLEND_INV_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case D3D12_BLEND_DEST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case D3D12_BLEND_INV_DEST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case D3D12_BLEND_DEST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case D3D12_BLEND_INV_DEST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case D3D12_BLEND_SRC_ALPHA_SAT: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case D3D12_BLEND_BLEND_FACTOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case D3D12_BLEND_INV_BLEND_FACTOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case D3D12_BLEND_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR;
    case D3D12_BLEND_INV_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case D3D12_BLEND_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case D3D12_BLEND_INV_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    default: return std::nullopt;
    }
}

std::optional<VkBlendOp> VulkanBlendOp(D3D12_BLEND_OP op) {
    switch (op) {
    case D3D12_BLEND_OP_ADD: return VK_BLEND_OP_ADD;
    case D3D12_BLEND_OP_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
    case D3D12_BLEND_OP_REV_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case D3D12_BLEND_OP_MIN: return VK_BLEND_OP_MIN;
    case D3D12_BLEND_OP_MAX: return VK_BLEND_OP_MAX;
    default: return std::nullopt;
    }
}

std::optional<VkLogicOp> VulkanLogicOp(D3D12_LOGIC_OP op) {
    switch (op) {
    case D3D12_LOGIC_OP_CLEAR: return VK_LOGIC_OP_CLEAR;
    case D3D12_LOGIC_OP_SET: return VK_LOGIC_OP_SET;
    case D3D12_LOGIC_OP_COPY: return VK_LOGIC_OP_COPY;
    case D3D12_LOGIC_OP_COPY_INVERTED: return VK_LOGIC_OP_COPY_INVERTED;
    case D3D12_LOGIC_OP_NOOP: return VK_LOGIC_OP_NO_OP;
    case D3D12_LOGIC_OP_INVERT: return VK_LOGIC_OP_INVERT;
    case D3D12_LOGIC_OP_AND: return VK_LOGIC_OP_AND;
    case D3D12_LOGIC_OP_NAND: return VK_LOGIC_OP_NAND;
    case D3D12_LOGIC_OP_OR: return VK_LOGIC_OP_OR;
    case D3D12_LOGIC_OP_NOR: return VK_LOGIC_OP_NOR;
    case D3D12_LOGIC_OP_XOR: return VK_LOGIC_OP_XOR;
    case D3D12_LOGIC_OP_EQUIV: return VK_LOGIC_OP_EQUIVALENT;
    case D3D12_LOGIC_OP_AND_REVERSE: return VK_LOGIC_OP_AND_REVERSE;
    case D3D12_LOGIC_OP_AND_INVERTED: return VK_LOGIC_OP_AND_INVERTED;
    case D3D12_LOGIC_OP_OR_REVERSE: return VK_LOGIC_OP_OR_REVERSE;
    case D3D12_LOGIC_OP_OR_INVERTED: return VK_LOGIC_OP_OR_INVERTED;
    default: return std::nullopt;
    }
}

std::optional<VkCompareOp> VulkanCompareOp(D3D12_COMPARISON_FUNC function) {
    switch (function) {
    case D3D12_COMPARISON_FUNC_NEVER: return VK_COMPARE_OP_NEVER;
    case D3D12_COMPARISON_FUNC_LESS: return VK_COMPARE_OP_LESS;
    case D3D12_COMPARISON_FUNC_EQUAL: return VK_COMPARE_OP_EQUAL;
    case D3D12_COMPARISON_FUNC_LESS_EQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case D3D12_COMPARISON_FUNC_GREATER: return VK_COMPARE_OP_GREATER;
    case D3D12_COMPARISON_FUNC_NOT_EQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case D3D12_COMPARISON_FUNC_GREATER_EQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case D3D12_COMPARISON_FUNC_ALWAYS: return VK_COMPARE_OP_ALWAYS;
    default: return std::nullopt;
    }
}

std::optional<VkStencilOp> VulkanStencilOp(D3D12_STENCIL_OP op) {
    switch (op) {
    case D3D12_STENCIL_OP_KEEP: return VK_STENCIL_OP_KEEP;
    case D3D12_STENCIL_OP_ZERO: return VK_STENCIL_OP_ZERO;
    case D3D12_STENCIL_OP_REPLACE: return VK_STENCIL_OP_REPLACE;
    case D3D12_STENCIL_OP_INCR_SAT: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case D3D12_STENCIL_OP_DECR_SAT: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case D3D12_STENCIL_OP_INVERT: return VK_STENCIL_OP_INVERT;
    case D3D12_STENCIL_OP_INCR: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case D3D12_STENCIL_OP_DECR: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return std::nullopt;
    }
}

std::optional<VkPrimitiveTopology> VulkanTopologyClass(D3D12_PRIMITIVE_TOPOLOGY_TYPE type) {
    switch (type) {
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE:
    case kConsoleRectangleTopologyType: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    default: return std::nullopt;
    }
}

std::optional<VkSampleCountFlagBits> VulkanSampleCount(UINT count) {
    switch (count) {
    case 1: return VK_SAMPLE_COUNT_1_BIT;
    case 2: return VK_SAMPLE_COUNT_2_BIT;
    case 4: return VK_SAMPLE_COUNT_4_BIT;
    case 8: return VK_SAMPLE_COUNT_8_BIT;
    case 16: return VK_SAMPLE_COUNT_16_BIT;
    default: return std::nullopt;
    }
}

std::optional<GraphicsPipelineState> TranslateGraphicsState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, std::string& error) {
    GraphicsPipelineState state;
    if (desc.HS.BytecodeLength != 0 || desc.DS.BytecodeLength != 0) {
        error = "tessellation (hull and domain shaders) is not supported";
        return std::nullopt;
    }
    if (desc.StreamOutput.NumEntries != 0) {
        error = "stream output is not supported";
        return std::nullopt;
    }
    if (desc.Flags != D3D12_PIPELINE_STATE_FLAG_NONE) {
        error = "pipeline state flags " + std::to_string(static_cast<unsigned>(desc.Flags)) + " are not supported";
        return std::nullopt;
    }
    if (desc.NodeMask > 1) {
        error = "node mask " + std::to_string(desc.NodeMask) + " names a GPU node other than the first";
        return std::nullopt;
    }
    const std::optional<VkPrimitiveTopology> topology = VulkanTopologyClass(desc.PrimitiveTopologyType);
    if (!topology) {
        error = "primitive topology type " + std::to_string(static_cast<int>(desc.PrimitiveTopologyType)) + " is not supported";
        return std::nullopt;
    }
    state.topology = *topology;
    if (desc.IBStripCutValue != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED && desc.IBStripCutValue != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF &&
        desc.IBStripCutValue != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF) {
        error = "strip cut value " + std::to_string(static_cast<int>(desc.IBStripCutValue)) + " is unknown";
        return std::nullopt;
    }
    state.stripCut = desc.IBStripCutValue != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;

    const D3D12_RASTERIZER_DESC& raster = desc.RasterizerState;
    if (raster.FillMode == D3D12_FILL_MODE_WIREFRAME) {
        state.polygonMode = VK_POLYGON_MODE_LINE;
        state.features.insert("fillModeNonSolid");
    } else if (raster.FillMode != D3D12_FILL_MODE_SOLID) {
        error = "fill mode " + std::to_string(static_cast<int>(raster.FillMode)) + " is unknown";
        return std::nullopt;
    }
    switch (raster.CullMode) {
    case D3D12_CULL_MODE_NONE: state.cullMode = VK_CULL_MODE_NONE; break;
    case D3D12_CULL_MODE_FRONT: state.cullMode = VK_CULL_MODE_FRONT_BIT; break;
    case D3D12_CULL_MODE_BACK: state.cullMode = VK_CULL_MODE_BACK_BIT; break;
    default:
        error = "cull mode " + std::to_string(static_cast<int>(raster.CullMode)) + " is unknown";
        return std::nullopt;
    }
    state.frontFace = raster.FrontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    if (!std::isfinite(raster.DepthBiasClamp) || !std::isfinite(raster.SlopeScaledDepthBias)) {
        error = "the depth bias is not finite";
        return std::nullopt;
    }
    state.depthBias = raster.DepthBias != 0 || raster.SlopeScaledDepthBias != 0.0f;
    state.depthBiasConstant = static_cast<float>(raster.DepthBias);
    state.depthBiasSlope = raster.SlopeScaledDepthBias;
    state.depthBiasClamp = state.depthBias ? raster.DepthBiasClamp : 0.0f;
    if (state.depthBiasClamp != 0.0f) {
        state.features.insert("depthBiasClamp");
    }
    state.depthClamp = !raster.DepthClipEnable;
    if (state.depthClamp) {
        state.features.insert("depthClamp");
    }
    if (raster.ForcedSampleCount != 0) {
        error = "a forced sample count is not supported";
        return std::nullopt;
    }
    state.conservative = raster.ConservativeRaster != D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    const std::optional<VkSampleCountFlagBits> samples = VulkanSampleCount(desc.SampleDesc.Count);
    if (!samples) {
        error = "sample count " + std::to_string(desc.SampleDesc.Count) + " is not supported";
        return std::nullopt;
    }
    if (desc.SampleDesc.Quality != 0 && desc.SampleDesc.Quality != kStandardMultisamplePattern) {
        error = "sample quality " + std::to_string(desc.SampleDesc.Quality) + " is not supported";
        return std::nullopt;
    }
    state.samples = *samples;
    state.sampleMask = desc.SampleMask;
    state.alphaToCoverage = desc.BlendState.AlphaToCoverageEnable != FALSE;

    if (desc.NumRenderTargets > kMaxColorTargets) {
        error = std::to_string(desc.NumRenderTargets) + " render targets are more than " + std::to_string(kMaxColorTargets);
        return std::nullopt;
    }
    const D3D12_BLEND_DESC& blend = desc.BlendState;
    for (UINT target = 0; target < desc.NumRenderTargets; ++target) {
        if (desc.RTVFormats[target] == DXGI_FORMAT_UNKNOWN) {
            state.colorFormats.push_back(VK_FORMAT_UNDEFINED);
        } else {
            const std::optional<VkFormat> format = VulkanFormat(desc.RTVFormats[target]);
            if (!format) {
                error = "render target " + std::to_string(target) + " has format " + std::to_string(static_cast<int>(desc.RTVFormats[target])) + ", which has no Vulkan mapping";
                return std::nullopt;
            }
            state.colorFormats.push_back(*format);
        }
        const D3D12_RENDER_TARGET_BLEND_DESC& source = blend.RenderTarget[blend.IndependentBlendEnable ? target : 0];
        if (blend.IndependentBlendEnable && target > 0 && !SameBlend(source, blend.RenderTarget[0])) {
            state.features.insert("independentBlend");
        }
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = source.RenderTargetWriteMask & (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
        attachment.blendEnable = source.BlendEnable ? VK_TRUE : VK_FALSE;
        if (source.BlendEnable) {
            const std::optional<VkBlendFactor> srcColor = VulkanBlendFactor(source.SrcBlend);
            const std::optional<VkBlendFactor> dstColor = VulkanBlendFactor(source.DestBlend);
            const std::optional<VkBlendFactor> srcAlpha = VulkanBlendFactor(source.SrcBlendAlpha);
            const std::optional<VkBlendFactor> dstAlpha = VulkanBlendFactor(source.DestBlendAlpha);
            const std::optional<VkBlendOp> colorOp = VulkanBlendOp(source.BlendOp);
            const std::optional<VkBlendOp> alphaOp = VulkanBlendOp(source.BlendOpAlpha);
            if (!srcColor || !dstColor || !srcAlpha || !dstAlpha || !colorOp || !alphaOp) {
                error = "render target " + std::to_string(target) + " has an unknown blend factor or operation";
                return std::nullopt;
            }
            attachment.srcColorBlendFactor = *srcColor;
            attachment.dstColorBlendFactor = *dstColor;
            attachment.colorBlendOp = *colorOp;
            attachment.srcAlphaBlendFactor = *srcAlpha;
            attachment.dstAlphaBlendFactor = *dstAlpha;
            attachment.alphaBlendOp = *alphaOp;
            if (IsDualSource(source.SrcBlend) || IsDualSource(source.DestBlend) || IsDualSource(source.SrcBlendAlpha) || IsDualSource(source.DestBlendAlpha)) {
                state.features.insert("dualSrcBlend");
            }
        }
        state.blend.push_back(attachment);
    }
    if (blend.RenderTarget[0].LogicOpEnable) {
        if (blend.RenderTarget[0].BlendEnable || blend.IndependentBlendEnable) {
            error = "a logic operation cannot be combined with blending or independent blend";
            return std::nullopt;
        }
        const std::optional<VkLogicOp> op = VulkanLogicOp(blend.RenderTarget[0].LogicOp);
        if (!op) {
            error = "logic operation " + std::to_string(static_cast<int>(blend.RenderTarget[0].LogicOp)) + " is unknown";
            return std::nullopt;
        }
        state.logicOpEnable = true;
        state.logicOp = *op;
        state.features.insert("logicOp");
    }

    if (desc.DSVFormat != DXGI_FORMAT_UNKNOWN) {
        const std::optional<VkFormat> format = VulkanFormat(desc.DSVFormat);
        if (!format || (desc.DSVFormat != DXGI_FORMAT_D16_UNORM && desc.DSVFormat != DXGI_FORMAT_D24_UNORM_S8_UINT && desc.DSVFormat != DXGI_FORMAT_D32_FLOAT &&
                        desc.DSVFormat != DXGI_FORMAT_D32_FLOAT_S8X24_UINT && desc.DSVFormat != kConsoleD16UnormS8Uint)) {
            error = "depth format " + std::to_string(static_cast<int>(desc.DSVFormat)) + " is not a depth format with a Vulkan mapping";
            return std::nullopt;
        }
        state.depthFormat = *format;
        state.stencilFormat = FormatHasStencil(desc.DSVFormat) ? *format : VK_FORMAT_UNDEFINED;
        const D3D12_DEPTH_STENCIL_DESC& depth = desc.DepthStencilState;
        if (depth.DepthEnable) {
            const std::optional<VkCompareOp> compare = VulkanCompareOp(depth.DepthFunc);
            if (!compare) {
                error = "depth comparison " + std::to_string(static_cast<int>(depth.DepthFunc)) + " is unknown";
                return std::nullopt;
            }
            state.depthTest = true;
            state.depthCompare = *compare;
            state.depthWrite = depth.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
        }
        if (depth.StencilEnable && state.stencilFormat != VK_FORMAT_UNDEFINED) {
            const std::optional<VkStencilOpState> front = StencilFace(depth.FrontFace, depth.StencilReadMask, depth.StencilWriteMask, error, "front");
            const std::optional<VkStencilOpState> back = front ? StencilFace(depth.BackFace, depth.StencilReadMask, depth.StencilWriteMask, error, "back") : std::nullopt;
            if (!front || !back) {
                return std::nullopt;
            }
            state.stencilTest = true;
            state.stencilFront = *front;
            state.stencilBack = *back;
        }
    }
    return state;
}

}
