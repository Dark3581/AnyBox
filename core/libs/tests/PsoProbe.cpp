#include "BindlessSet.hpp"
#include "BufferArena.hpp"
#include "BufferDescriptor.hpp"
#include "CommandQueue.hpp"
#include "CommandRecorder.hpp"
#include "CopyLayout.hpp"
#include "Fence.hpp"
#include "GraphicsPipeline.hpp"
#include "GraphicsState.hpp"
#include "PipelineCompiler.hpp"
#include "PipelineLibrary.hpp"
#include "PipelineState.hpp"
#include "Resource.hpp"
#include "ResourceState.hpp"
#include "RootSignatureLayout.hpp"
#include "ShaderStore.hpp"
#include "TextureDesc.hpp"
#include "TitleBufferWindow.hpp"
#include "VertexInput.hpp"
#include "ViewDesc.hpp"
#include "VulkanDevice.hpp"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace abx::d3d12x;

constexpr int kNoDevice = 3;

const std::uint32_t kGraphicsVertex[] = {
#include "shaders/graphics_test.inc"
};

const std::uint32_t kGraphicsFloat[] = {
#include "shaders/graphics_float.inc"
};

const std::uint32_t kGraphicsUint[] = {
#include "shaders/graphics_uint.inc"
};

constexpr int kNoValidation = 4;

int Fail(const std::string& what) {
    std::printf("FAIL %s\n", what.c_str());
    return 1;
}

D3D12_GRAPHICS_PIPELINE_STATE_DESC DefaultGraphics() {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    for (D3D12_RENDER_TARGET_BLEND_DESC& target : desc.BlendState.RenderTarget) {
        target = {FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
    }
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.DepthStencilState.StencilReadMask = 0xFF;
    desc.DepthStencilState.StencilWriteMask = 0xFF;
    desc.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;
    desc.SampleMask = 0xFFFFFFFFu;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;
    return desc;
}

struct GraphicsCase {
    const char* name;
    bool uintTarget;
    void (*edit)(D3D12_GRAPHICS_PIPELINE_STATE_DESC&);
};

const GraphicsCase kGraphicsCases[] = {
    {"opaque", false, [](D3D12_GRAPHICS_PIPELINE_STATE_DESC&) {}},
    {"mrt-blend-stencil", false, [](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
         d.NumRenderTargets = 3;
         d.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
         d.RTVFormats[1] = DXGI_FORMAT_R11G11B10_FLOAT;
         d.RTVFormats[2] = DXGI_FORMAT_R10G10B10A2_UNORM;
         d.BlendState.IndependentBlendEnable = TRUE;
         d.BlendState.RenderTarget[0] = {TRUE, FALSE, D3D12_BLEND_SRC_ALPHA, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
         d.BlendState.RenderTarget[1] = {TRUE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_OP_MAX, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL};
         d.BlendState.RenderTarget[2] = {TRUE, FALSE, D3D12_BLEND_BLEND_FACTOR, D3D12_BLEND_INV_BLEND_FACTOR, D3D12_BLEND_OP_REV_SUBTRACT, D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, 7};
         d.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
         d.DepthStencilState.StencilEnable = TRUE;
         d.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_INCR, D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS};
         d.DepthStencilState.BackFace = {D3D12_STENCIL_OP_ZERO, D3D12_STENCIL_OP_DECR_SAT, D3D12_STENCIL_OP_INVERT, D3D12_COMPARISON_FUNC_NOT_EQUAL};
     }},
    {"wireframe-bias-lines", false, [](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
         d.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
         d.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
         d.RasterizerState.FrontCounterClockwise = TRUE;
         d.RasterizerState.DepthBias = 100;
         d.RasterizerState.SlopeScaledDepthBias = 1.5f;
         d.RasterizerState.DepthBiasClamp = 0.01f;
         d.RasterizerState.DepthClipEnable = FALSE;
         d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
     }},
    {"msaa-points", false, [](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
         d.SampleDesc = {4, 0xFFFFFFFFu};
         d.SampleMask = 0x7;
         d.BlendState.AlphaToCoverageEnable = TRUE;
         d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
         d.DSVFormat = DXGI_FORMAT_D16_UNORM;
         d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
     }},
    {"logic-xor", true, [](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
         d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UINT;
         d.BlendState.RenderTarget[0].LogicOpEnable = TRUE;
         d.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_XOR;
         d.DSVFormat = DXGI_FORMAT_UNKNOWN;
     }},
    {"depth-only-strip-cut", false, [](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
         d.NumRenderTargets = 0;
         d.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
         d.DSVFormat = DXGI_FORMAT_D16_UNORM;
         d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
         d.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
     }},
    {"target-gap", false, [](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
         d.NumRenderTargets = 3;
         d.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
         d.RTVFormats[2] = DXGI_FORMAT_B8G8R8A8_UNORM;
         d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
     }},
};

const std::uint32_t kGraphicsInputs[] = {
#include "shaders/graphics_inputs.inc"
};

const std::vector<ShaderInput>& TestShaderInputs() {
    static const std::vector<ShaderInput> inputs{
        {0, "POSITION", 0, InputType::Float, 7},
        {1, "COLOR", 0, InputType::Float, 15},
        {3, "BLENDINDICES", 0, InputType::Uint, 15},
    };
    return inputs;
}

const D3D12_INPUT_ELEMENT_DESC kTestLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"color", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 2, 4, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
};

int InputSelfTest() {
    std::string error;
    const D3D12_INPUT_LAYOUT_DESC layout{kTestLayout, 4};
    const std::optional<VertexInput> input = TranslateInputLayout(layout, TestShaderInputs(), error);
    if (!input) return Fail("test layout: " + error);
    if (input->attributes.size() != 3 || input->bindings.size() != 2) return Fail("test layout counts");
    const VkVertexInputAttributeDescription& position = input->attributes[0];
    const VkVertexInputAttributeDescription& color = input->attributes[1];
    const VkVertexInputAttributeDescription& bones = input->attributes[2];
    if (position.location != 0 || position.binding != 0 || position.format != VK_FORMAT_R32G32B32_SFLOAT || position.offset != 0) return Fail("position");
    if (color.location != 1 || color.binding != 0 || color.format != VK_FORMAT_R8G8B8A8_UNORM || color.offset != 12) return Fail("append aligned colour");
    if (bones.location != 3 || bones.binding != 2 || bones.format != VK_FORMAT_R8G8B8A8_UINT || bones.offset != 4) return Fail("instance element");
    if (input->bindings[0].binding != 0 || input->bindings[0].inputRate != VK_VERTEX_INPUT_RATE_VERTEX || input->bindings[1].binding != 2 ||
        input->bindings[1].inputRate != VK_VERTEX_INPUT_RATE_INSTANCE) return Fail("bindings");
    const D3D12_INPUT_ELEMENT_DESC packed[] = {
        {"A", 0, DXGI_FORMAT_R8_UNORM, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"B", 0, DXGI_FORMAT_R16_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"C", 0, DXGI_FORMAT_R8_UINT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"D", 0, DXGI_FORMAT_R32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    const std::vector<ShaderInput> packedInputs{{0, "A", 0, InputType::Float, 1}, {1, "B", 0, InputType::Float, 1}, {2, "C", 0, InputType::Uint, 1}, {3, "D", 0, InputType::Float, 1}};
    const std::optional<VertexInput> packedInput = TranslateInputLayout({packed, 4}, packedInputs, error);
    if (!packedInput || packedInput->attributes[1].offset != 2 || packedInput->attributes[2].offset != 4 || packedInput->attributes[3].offset != 8) return Fail("append aligned packing: " + error);
    const std::optional<VertexInput> empty = TranslateInputLayout({nullptr, 0}, {}, error);
    if (!empty || !empty->attributes.empty() || !empty->bindings.empty()) return Fail("empty layout");
    if (VertexFormatBytes(DXGI_FORMAT_R16G16B16A16_SNORM) != 8u || VertexFormatBytes(DXGI_FORMAT_R11G11B10_FLOAT) != 4u || VertexFormatBytes(DXGI_FORMAT_BC1_UNORM)) return Fail("format sizes");
    if (VertexFormatType(DXGI_FORMAT_R16G16_SINT) != InputType::Sint || VertexFormatType(DXGI_FORMAT_R10G10B10A2_UINT) != InputType::Uint ||
        VertexFormatType(DXGI_FORMAT_R8G8B8A8_SNORM) != InputType::Float) return Fail("format types");

    struct Refusal {
        void (*edit)(std::vector<D3D12_INPUT_ELEMENT_DESC>&);
        const char* expected;
    };
    const Refusal refusals[] = {
        {[](std::vector<D3D12_INPUT_ELEMENT_DESC>& e) { e.erase(e.begin() + 1); }, "no element for COLOR0, which the vertex shader reads"},
        {[](std::vector<D3D12_INPUT_ELEMENT_DESC>& e) { e[3].Format = DXGI_FORMAT_R8G8B8A8_UNORM; }, "reads BLENDINDICES0 as uint, but the input layout gives it format 28"},
        {[](std::vector<D3D12_INPUT_ELEMENT_DESC>& e) { e[3].InstanceDataStepRate = 2; }, "instance step rate 2, and only 1 is supported"},
        {[](std::vector<D3D12_INPUT_ELEMENT_DESC>& e) { e[0].InstanceDataStepRate = 1; }, "per-vertex input element POSITION0 has instance step rate 1"},
        {[](std::vector<D3D12_INPUT_ELEMENT_DESC>& e) { e[3].InputSlot = 0; }, "slot 0 mixes per-vertex and per-instance elements"},
        {[](std::vector<D3D12_INPUT_ELEMENT_DESC>& e) { e[0].InputSlot = 32; }, "uses slot 32"},
        {[](std::vector<D3D12_INPUT_ELEMENT_DESC>& e) { e[0].Format = DXGI_FORMAT_BC1_UNORM; }, "has format 71, which has no Vulkan vertex mapping"},
        {[](std::vector<D3D12_INPUT_ELEMENT_DESC>& e) { e[2] = e[1]; }, "names color0 twice"},
        {[](std::vector<D3D12_INPUT_ELEMENT_DESC>& e) { e[0].SemanticName = nullptr; }, "input element 0 has no semantic name"},
        {[](std::vector<D3D12_INPUT_ELEMENT_DESC>& e) { e[0].AlignedByteOffset = 4096; }, "starts at byte 4096"},
        {[](std::vector<D3D12_INPUT_ELEMENT_DESC>& e) { e[0].InputSlotClass = static_cast<D3D12_INPUT_CLASSIFICATION>(5); }, "has classification 5"},
    };
    for (const Refusal& refusal : refusals) {
        std::vector<D3D12_INPUT_ELEMENT_DESC> elements(std::begin(kTestLayout), std::end(kTestLayout));
        refusal.edit(elements);
        error.clear();
        if (TranslateInputLayout({elements.data(), static_cast<UINT>(elements.size())}, TestShaderInputs(), error) || error.find(refusal.expected) == std::string::npos) {
            return Fail(std::string("refusal '") + refusal.expected + "' gave '" + error + "'");
        }
    }
    std::vector<D3D12_INPUT_ELEMENT_DESC> many(33, kTestLayout[0]);
    if (TranslateInputLayout({many.data(), 33}, {}, error) || error.find("33 input elements") == std::string::npos) return Fail("too many elements");
    if (TranslateInputLayout({nullptr, 2}, {}, error) || error.find("no element array") == std::string::npos) return Fail("missing element array");
    std::puts("PASS input selftest");
    return 0;
}

D3D12_INPUT_ELEMENT_DESC SynthesizedElement(const ShaderInput& input) {
    unsigned components = 1;
    for (unsigned bit = 0; bit < 4; ++bit) {
        if ((input.mask >> bit) & 1u) components = bit + 1;
    }
    static const DXGI_FORMAT formats[3][4] = {
        {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT},
        {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32B32_UINT, DXGI_FORMAT_R32G32B32A32_UINT},
        {DXGI_FORMAT_R32_SINT, DXGI_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32B32_SINT, DXGI_FORMAT_R32G32B32A32_SINT},
    };
    const int type = input.type == InputType::Float ? 0 : input.type == InputType::Uint ? 1 : 2;
    return {input.semantic.c_str(), input.index, formats[type][components - 1], 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
}

int GraphicsSelfTest() {
    if (VulkanFormat(DXGI_FORMAT_R8G8B8A8_UNORM) != VK_FORMAT_R8G8B8A8_UNORM) return Fail("RGBA8");
    if (VulkanFormat(DXGI_FORMAT_R10G10B10A2_UNORM) != VK_FORMAT_A2B10G10R10_UNORM_PACK32) return Fail("RGB10A2 packs red lowest");
    if (VulkanFormat(DXGI_FORMAT_R11G11B10_FLOAT) != VK_FORMAT_B10G11R11_UFLOAT_PACK32) return Fail("R11G11B10 packs red lowest");
    if (VulkanFormat(DXGI_FORMAT_B5G6R5_UNORM) != VK_FORMAT_R5G6B5_UNORM_PACK16) return Fail("B5G6R5 packs blue lowest");
    if (VulkanFormat(DXGI_FORMAT_R9G9B9E5_SHAREDEXP) != VK_FORMAT_E5B9G9R9_UFLOAT_PACK32) return Fail("shared exponent");
    if (VulkanFormat(DXGI_FORMAT_D32_FLOAT_S8X24_UINT) != VK_FORMAT_D32_SFLOAT_S8_UINT || !FormatHasStencil(DXGI_FORMAT_D32_FLOAT_S8X24_UINT)) return Fail("D32S8");
    if (FormatHasStencil(DXGI_FORMAT_D32_FLOAT) || !FormatHasStencil(DXGI_FORMAT_D24_UNORM_S8_UINT)) return Fail("stencil formats");
    if (VulkanFormat(DXGI_FORMAT_R8G8B8A8_TYPELESS) || VulkanFormat(static_cast<DXGI_FORMAT>(200)) || VulkanFormat(DXGI_FORMAT_UNKNOWN)) return Fail("typeless and unknown formats");
    std::set<int> seen;
    for (int op = D3D12_LOGIC_OP_CLEAR; op <= D3D12_LOGIC_OP_OR_INVERTED; ++op) {
        const std::optional<VkLogicOp> mapped = VulkanLogicOp(static_cast<D3D12_LOGIC_OP>(op));
        if (!mapped) return Fail("logic op " + std::to_string(op));
        seen.insert(*mapped);
    }
    if (seen.size() != 16 || VulkanLogicOp(D3D12_LOGIC_OP_EQUIV) != VK_LOGIC_OP_EQUIVALENT || VulkanLogicOp(D3D12_LOGIC_OP_NOOP) != VK_LOGIC_OP_NO_OP) return Fail("logic ops");
    seen.clear();
    for (int function = D3D12_COMPARISON_FUNC_NEVER; function <= D3D12_COMPARISON_FUNC_ALWAYS; ++function) {
        const std::optional<VkCompareOp> mapped = VulkanCompareOp(static_cast<D3D12_COMPARISON_FUNC>(function));
        if (!mapped) return Fail("compare " + std::to_string(function));
        seen.insert(*mapped);
    }
    if (seen.size() != 8 || VulkanCompareOp(D3D12_COMPARISON_FUNC_GREATER_EQUAL) != VK_COMPARE_OP_GREATER_OR_EQUAL || VulkanCompareOp(static_cast<D3D12_COMPARISON_FUNC>(0))) return Fail("compare ops");
    seen.clear();
    for (int op = D3D12_STENCIL_OP_KEEP; op <= D3D12_STENCIL_OP_DECR; ++op) {
        const std::optional<VkStencilOp> mapped = VulkanStencilOp(static_cast<D3D12_STENCIL_OP>(op));
        if (!mapped) return Fail("stencil op " + std::to_string(op));
        seen.insert(*mapped);
    }
    if (seen.size() != 8 || VulkanStencilOp(D3D12_STENCIL_OP_INCR) != VK_STENCIL_OP_INCREMENT_AND_WRAP || VulkanStencilOp(D3D12_STENCIL_OP_DECR_SAT) != VK_STENCIL_OP_DECREMENT_AND_CLAMP) return Fail("stencil ops");
    if (VulkanBlendFactor(D3D12_BLEND_BLEND_FACTOR) != VK_BLEND_FACTOR_CONSTANT_COLOR || VulkanBlendFactor(D3D12_BLEND_SRC_ALPHA_SAT) != VK_BLEND_FACTOR_SRC_ALPHA_SATURATE ||
        VulkanBlendFactor(D3D12_BLEND_INV_SRC1_ALPHA) != VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA || VulkanBlendFactor(static_cast<D3D12_BLEND>(12))) return Fail("blend factors");
    if (VulkanBlendOp(D3D12_BLEND_OP_REV_SUBTRACT) != VK_BLEND_OP_REVERSE_SUBTRACT || VulkanBlendOp(static_cast<D3D12_BLEND_OP>(6))) return Fail("blend ops");
    if (VulkanTopologyClass(D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH) || VulkanTopologyClass(D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED) || VulkanSampleCount(3) || VulkanSampleCount(32)) return Fail("topology and samples");

    std::string error;
    std::optional<GraphicsPipelineState> state = TranslateGraphicsState(DefaultGraphics(), error);
    if (!state) return Fail("default: " + error);
    if (state->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || state->cullMode != VK_CULL_MODE_BACK_BIT || state->frontFace != VK_FRONT_FACE_CLOCKWISE || state->depthClamp ||
        state->depthBias || !state->depthTest || !state->depthWrite || state->depthCompare != VK_COMPARE_OP_LESS || state->stencilTest || state->colorFormats.size() != 1 ||
        state->blend.size() != 1 || state->blend[0].blendEnable || state->blend[0].colorWriteMask != 0xF || state->depthFormat != VK_FORMAT_D32_SFLOAT ||
        state->stencilFormat != VK_FORMAT_UNDEFINED || !state->features.empty() || state->stripCut) return Fail("default state");
    for (const GraphicsCase& test : kGraphicsCases) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = DefaultGraphics();
        test.edit(desc);
        if (!TranslateGraphicsState(desc, error)) return Fail(std::string(test.name) + ": " + error);
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = DefaultGraphics();
    kGraphicsCases[1].edit(desc);
    state = TranslateGraphicsState(desc, error);
    if (!state->features.count("independentBlend") || state->stencilFormat != VK_FORMAT_D32_SFLOAT_S8_UINT || !state->stencilTest ||
        state->stencilBack.passOp != VK_STENCIL_OP_INVERT || state->stencilBack.compareOp != VK_COMPARE_OP_NOT_EQUAL || state->blend[2].colorWriteMask != 7 ||
        state->blend[1].alphaBlendOp != VK_BLEND_OP_MAX) return Fail("blend and stencil state");
    desc = DefaultGraphics();
    kGraphicsCases[2].edit(desc);
    state = TranslateGraphicsState(desc, error);
    if (state->polygonMode != VK_POLYGON_MODE_LINE || !state->depthClamp || !state->depthBias || state->depthBiasConstant != 100.0f || state->frontFace != VK_FRONT_FACE_COUNTER_CLOCKWISE ||
        state->features != std::set<std::string>{"fillModeNonSolid", "depthClamp", "depthBiasClamp"} || state->topology != VK_PRIMITIVE_TOPOLOGY_LINE_LIST) return Fail("raster state");
    desc = DefaultGraphics();
    desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    state = TranslateGraphicsState(desc, error);
    if (state->depthTest || state->depthWrite || state->depthFormat != VK_FORMAT_UNDEFINED) return Fail("depth without a depth target");
    desc = DefaultGraphics();
    desc.DepthStencilState.StencilEnable = TRUE;
    state = TranslateGraphicsState(desc, error);
    if (state->stencilTest) return Fail("stencil without a stencil format");

    const std::pair<void (*)(D3D12_GRAPHICS_PIPELINE_STATE_DESC&), const char*> refusals[] = {
        {[](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) { d.HS.BytecodeLength = 4; }, "tessellation"},
        {[](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) { d.StreamOutput.NumEntries = 1; }, "stream output"},
        {[](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) { d.Flags = static_cast<D3D12_PIPELINE_STATE_FLAGS>(1); }, "pipeline state flags 1"},
        {[](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) { d.NodeMask = 2; }, "node mask 2"},
        {[](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) { d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH; }, "primitive topology type 4"},
        {[](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) { d.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON; }, "conservative"},
        {[](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) { d.RasterizerState.ForcedSampleCount = 4; }, "forced sample count"},
        {[](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) { d.RasterizerState.FillMode = static_cast<D3D12_FILL_MODE>(7); }, "fill mode 7"},
        {[](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) { d.SampleDesc.Count = 3; }, "sample count 3"},
        {[](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) { d.SampleDesc.Quality = 2; }, "sample quality 2"},
        {[](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) { d.NumRenderTargets = 9; }, "9 render targets"},
        {[](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) { d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_TYPELESS; }, "render target 0 has format 27"},
        {[](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) { d.RTVFormats[0] = static_cast<DXGI_FORMAT>(200); }, "has format 200"},
        {[](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) { d.DSVFormat = DXGI_FORMAT_R32_FLOAT; }, "not a depth format"},
        {[](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) { d.DepthStencilState.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(0); }, "depth comparison 0"},
        {[](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) { d.BlendState.RenderTarget[0].BlendEnable = TRUE; d.BlendState.RenderTarget[0].SrcBlend = static_cast<D3D12_BLEND>(12); }, "unknown blend"},
        {[](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) { d.BlendState.RenderTarget[0].LogicOpEnable = TRUE; d.BlendState.RenderTarget[0].BlendEnable = TRUE; }, "logic operation cannot"},
        {[](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) { d.IBStripCutValue = static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(3); }, "strip cut value 3"},
        {[](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) { d.RasterizerState.SlopeScaledDepthBias = std::numeric_limits<float>::infinity(); }, "not finite"},
        {[](D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
             d.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
             d.DepthStencilState.StencilEnable = TRUE;
             d.DepthStencilState.BackFace.StencilPassOp = static_cast<D3D12_STENCIL_OP>(9);
         }, "back stencil face"},
    };
    for (const auto& [edit, expected] : refusals) {
        desc = DefaultGraphics();
        edit(desc);
        error.clear();
        if (TranslateGraphicsState(desc, error) || error.find(expected) == std::string::npos) return Fail(std::string("refusal '") + expected + "' gave '" + error + "'");
    }
    std::printf("dynamic states %zu\n", GraphicsDynamicStates().size());
    std::puts("PASS graphics selftest");
    return 0;
}

int GraphicsGpu(bool validate) {
    DeviceRequirements requirements;
    requirements.validation = validate;
    std::string error;
    const std::unique_ptr<VulkanDevice> device = VulkanDevice::Create(requirements, error);
    if (!device) {
        std::printf("SKIP no usable Vulkan device: %s\n", error.c_str());
        return validate && error == "the Khronos validation layer is not installed" ? kNoValidation : kNoDevice;
    }
    std::printf("device %s\n", device->Gpu().name.c_str());
    const DeviceFunctions& vk = device->Vk();
    const auto makeModule = [&](const std::uint32_t* words, std::size_t bytes) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = bytes;
        info.pCode = words;
        VkShaderModule module = VK_NULL_HANDLE;
        return vk.createShaderModule(device->Handle(), &info, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
    };
    const VkShaderModule vertex = makeModule(kGraphicsVertex, sizeof(kGraphicsVertex));
    const VkShaderModule floatFragment = makeModule(kGraphicsFloat, sizeof(kGraphicsFloat));
    const VkShaderModule uintFragment = makeModule(kGraphicsUint, sizeof(kGraphicsUint));
    if (vertex == VK_NULL_HANDLE || floatFragment == VK_NULL_HANDLE || uintFragment == VK_NULL_HANDLE) return Fail("test shader modules");
    PipelineCache cache(*device, {});
    std::size_t created = 0;
    std::size_t skipped = 0;
    int status = 0;
    for (const GraphicsCase& test : kGraphicsCases) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = DefaultGraphics();
        test.edit(desc);
        const std::optional<GraphicsPipelineState> state = TranslateGraphicsState(desc, error);
        if (!state) {
            status = Fail(std::string(test.name) + ": " + error);
            continue;
        }
        std::string missing;
        for (const std::string& feature : state->features) {
            if (!device->HasFeature(feature)) missing += (missing.empty() ? "" : ",") + feature;
        }
        if (!missing.empty()) {
            std::printf("SKIPPED %s: the device lacks %s\n", test.name, missing.c_str());
            ++skipped;
            continue;
        }
        const std::vector<GraphicsStage> stages{{VK_SHADER_STAGE_VERTEX_BIT, vertex, 0}, {VK_SHADER_STAGE_FRAGMENT_BIT, test.uintTarget ? uintFragment : floatFragment, 0}};
        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult result = CreateGraphicsPipeline(*device, cache.Handle(), *state, stages, {}, 0, &pipeline, error);
        if (result != VK_SUCCESS) {
            status = Fail(std::string(test.name) + ": " + error);
            continue;
        }
        std::printf("OK %s\n", test.name);
        vk.destroyPipeline(device->Handle(), pipeline, nullptr);
        ++created;
    }
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = DefaultGraphics();
        const std::optional<GraphicsPipelineState> state = TranslateGraphicsState(desc, error);
        const D3D12_INPUT_LAYOUT_DESC layout{kTestLayout, 4};
        const std::optional<VertexInput> input = state ? TranslateInputLayout(layout, TestShaderInputs(), error) : std::nullopt;
        const VkShaderModule inputVertex = makeModule(kGraphicsInputs, sizeof(kGraphicsInputs));
        VkPipeline inputPipeline = VK_NULL_HANDLE;
        if (!input || inputVertex == VK_NULL_HANDLE ||
            CreateGraphicsPipeline(*device, cache.Handle(), *state, {{VK_SHADER_STAGE_VERTEX_BIT, inputVertex, 0}, {VK_SHADER_STAGE_FRAGMENT_BIT, floatFragment, 0}}, *input, 0,
                                   &inputPipeline, error) != VK_SUCCESS) {
            status = Fail("vertex-input: " + error);
        } else {
            std::puts("OK vertex-input");
            vk.destroyPipeline(device->Handle(), inputPipeline, nullptr);
            ++created;
        }
        if (inputVertex != VK_NULL_HANDLE) vk.destroyShaderModule(device->Handle(), inputVertex, nullptr);
    }
    GraphicsPipelineState noVertex;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (CreateGraphicsPipeline(*device, cache.Handle(), noVertex, {{VK_SHADER_STAGE_FRAGMENT_BIT, floatFragment, 0}}, {}, 0, &pipeline, error) == VK_SUCCESS ||
        error != "the pipeline has no vertex shader") status = Fail("no vertex shader: " + error);
    GraphicsPipelineState needy;
    needy.features.insert("notAFeature");
    if (CreateGraphicsPipeline(*device, cache.Handle(), needy, {{VK_SHADER_STAGE_VERTEX_BIT, vertex, 0}}, {}, 0, &pipeline, error) != VK_ERROR_FEATURE_NOT_PRESENT) status = Fail("missing feature");
    vk.destroyShaderModule(device->Handle(), vertex, nullptr);
    vk.destroyShaderModule(device->Handle(), floatFragment, nullptr);
    vk.destroyShaderModule(device->Handle(), uintFragment, nullptr);
    std::printf("graphics pipelines: %zu created, %zu skipped for missing features\n", created, skipped);
    if (validate) {
        std::printf("validation errors=%zu\n", device->ValidationErrors());
        for (const std::string& message : device->ValidationMessages()) std::printf("  %.600s\n", message.c_str());
        if (device->ValidationErrors() != 0) status = 1;
    }
    if (status == 0) std::puts("PASS graphics gpu");
    return status;
}


std::filesystem::path Utf8Path(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string FindOption(int argc, char** argv, const char* name, const char* fallback) {
    for (int index = 2; index + 1 < argc; ++index) {
        if (std::strcmp(argv[index], name) == 0) {
            return argv[index + 1];
        }
    }
    return fallback;
}

bool HasFlag(int argc, char** argv, const char* name) {
    for (int index = 2; index < argc; ++index) {
        if (std::strcmp(argv[index], name) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<unsigned char> ReadBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

bool LoadStore(const std::string& folder, ShaderStore& store) {
    std::string error;
    if (!store.Load(Utf8Path(folder).wstring(), error)) {
        std::printf("FAIL store: %s\n", error.c_str());
        return false;
    }
    return true;
}

D3D12_COMPUTE_PIPELINE_STATE_DESC Description(const std::vector<unsigned char>& bytecode) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.CS.pShaderBytecode = bytecode.empty() ? nullptr : bytecode.data();
    desc.CS.BytecodeLength = bytecode.size();
    return desc;
}

int Resolve(int argc, char** argv) {
    ShaderStore store;
    if (!LoadStore(argv[2], store)) return 1;
    std::vector<unsigned char> bytecode;
    if (std::strcmp(argv[3], "none") != 0) {
        bytecode = ReadBytes(Utf8Path(argv[3]));
        bytecode.resize(bytecode.size() + 5, 0);
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = Description(bytecode);
    desc.Flags = static_cast<D3D12_PIPELINE_STATE_FLAGS>(std::strtoul(FindOption(argc, argv, "--flags", "0").c_str(), nullptr, 0));
    desc.NodeMask = static_cast<UINT>(std::strtoul(FindOption(argc, argv, "--node", "0").c_str(), nullptr, 0));
    const std::vector<unsigned char> blob(std::strtoul(FindOption(argc, argv, "--cached", "0").c_str(), nullptr, 0), 0xCC);
    desc.CachedPSO.pCachedBlob = blob.empty() ? nullptr : blob.data();
    desc.CachedPSO.CachedBlobSizeInBytes = blob.size();
    std::string error;
    const std::optional<PipelineSource> source = ComputePipelineSource(desc, store, error);
    if (!source) {
        std::printf("error\t%s\n", error.c_str());
        return 0;
    }
    const std::u8string module = source->module.u8string();
    std::printf("ok\t%s\t%u\t%s\n", source->key.c_str(), source->subgroupSize, std::string(module.begin(), module.end()).c_str());
    return 0;
}

std::vector<std::string> Lines(const std::filesystem::path& path) {
    std::vector<std::string> lines;
    std::ifstream file(path, std::ios::binary);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

struct Row {
    PipelineEntry entry;
    std::string error;
};

int Compute(int argc, char** argv) {
    ShaderStore store;
    if (!LoadStore(argv[2], store)) return 1;
    const std::vector<std::string> containers = Lines(Utf8Path(argv[3]));
    if (containers.empty()) {
        std::puts("FAIL no containers listed");
        return 1;
    }
    DeviceRequirements requirements;
    std::istringstream features(FindOption(argc, argv, "--features", ""));
    for (std::string feature; std::getline(features, feature, ',');) {
        if (!feature.empty()) requirements.features.insert(feature);
    }
    requirements.maintenance8 = HasFlag(argc, argv, "--maintenance8");
    std::string error;
    const std::unique_ptr<VulkanDevice> device = VulkanDevice::Create(requirements, error);
    if (!device) {
        std::printf("SKIP no usable Vulkan device: %s\n", error.c_str());
        return kNoDevice;
    }
    std::printf("device %s, pipeline cache control %s\n", device->Gpu().name.c_str(), device->PipelineCacheControl() ? "yes" : "no");
    PipelineCache cache(*device, Utf8Path(FindOption(argc, argv, "--cache", "")));
    if (!cache.Valid()) {
        std::puts("FAIL pipeline cache");
        return 1;
    }
    std::printf("cache seeded with %zu bytes%s%s\n", cache.SeedBytes(), cache.Note().empty() ? "" : "; ", cache.Note().c_str());
    std::vector<Row> rows(containers.size());
    {
        PipelineLibrary library(*device, cache.Handle(), std::strtoull(FindOption(argc, argv, "--limit", "0").c_str(), nullptr, 10));
        std::atomic<std::size_t> next{0};
        const unsigned threads = static_cast<unsigned>((std::max)(1ul, std::strtoul(FindOption(argc, argv, "--threads", "1").c_str(), nullptr, 10)));
        std::vector<std::thread> pool;
        for (unsigned worker = 0; worker < threads; ++worker) {
            pool.emplace_back([&] {
                for (std::size_t index = next++; index < containers.size(); index = next++) {
                    const std::vector<unsigned char> bytecode = ReadBytes(Utf8Path(containers[index]));
                    std::string reason;
                    const std::optional<PipelineSource> source = ComputePipelineSource(Description(bytecode), store, reason);
                    if (!source) {
                        rows[index].error = reason;
                        continue;
                    }
                    rows[index].entry = library.Compute(*source);
                    rows[index].error = rows[index].entry.error;
                }
            });
        }
        for (std::thread& thread : pool) thread.join();
        for (std::size_t index = 0; index < containers.size(); ++index) {
            const PipelineEntry& entry = rows[index].entry;
            std::printf("%s\t%s\t%d\t%llu\t%.3f\t%s\n", containers[index].c_str(), PipelineOriginName(entry.origin), entry.driverCacheHit ? 1 : 0,
                        static_cast<unsigned long long>(entry.bytes), entry.seconds, rows[index].error.c_str());
        }
        const PipelineLibraryStats stats = library.Stats();
        std::printf("stats ready=%zu compiled=%zu refused=%zu failed=%zu\n", stats.ready, stats.compiled, stats.refused, stats.failed);
    }
    if (HasFlag(argc, argv, "--save")) {
        std::size_t written = 0;
        if (!cache.Save(written, error)) {
            std::printf("FAIL cache save: %s\n", error.c_str());
            return 1;
        }
        std::printf("cache saved, %zu bytes\n", written);
    }
    return 0;
}

int GraphicsTitle(int argc, char** argv) {
    ShaderStore store;
    if (!LoadStore(argv[2], store)) return 1;
    const std::vector<unsigned char> bytecode = ReadBytes(Utf8Path(argv[3]));
    const ShaderModule* module = store.Find(bytecode.data(), bytecode.size());
    if (module == nullptr || module->stage != "vs") return Fail("the container is not a vertex shader in the store");
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    for (const ShaderInput& input : module->inputs) elements.push_back(SynthesizedElement(input));
    std::string error;
    const std::optional<VertexInput> input = TranslateInputLayout({elements.empty() ? nullptr : elements.data(), static_cast<UINT>(elements.size())}, module->inputs, error);
    if (!input) return Fail("input layout: " + error);
    const std::optional<GraphicsPipelineState> state = TranslateGraphicsState(DefaultGraphics(), error);
    if (!state) return Fail("state: " + error);
    DeviceRequirements requirements;
    std::istringstream features(FindOption(argc, argv, "--features", ""));
    for (std::string feature; std::getline(features, feature, ',');) {
        if (!feature.empty()) requirements.features.insert(feature);
    }
    requirements.maintenance8 = HasFlag(argc, argv, "--maintenance8");
    const std::unique_ptr<VulkanDevice> device = VulkanDevice::Create(requirements, error);
    if (!device) {
        std::printf("SKIP no usable Vulkan device: %s\n", error.c_str());
        return kNoDevice;
    }
    const std::optional<std::vector<std::uint32_t>> words = ReadSpirv(std::filesystem::path(module->path), error);
    if (!words) return Fail(error);
    const DeviceFunctions& vk = device->Vk();
    const auto makeModule = [&](const std::uint32_t* code, std::size_t bytes) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = bytes;
        info.pCode = code;
        VkShaderModule handle = VK_NULL_HANDLE;
        return vk.createShaderModule(device->Handle(), &info, nullptr, &handle) == VK_SUCCESS ? handle : VK_NULL_HANDLE;
    };
    const VkShaderModule vertex = makeModule(words->data(), words->size() * sizeof(std::uint32_t));
    const VkShaderModule fragment = makeModule(kGraphicsFloat, sizeof(kGraphicsFloat));
    if (vertex == VK_NULL_HANDLE || fragment == VK_NULL_HANDLE) return Fail("shader modules");
    PipelineCache cache(*device, {});
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = CreateGraphicsPipeline(*device, cache.Handle(), *state, {{VK_SHADER_STAGE_VERTEX_BIT, vertex, module->subgroupSize}, {VK_SHADER_STAGE_FRAGMENT_BIT, fragment, 0}},
                                                   *input, 0, &pipeline, error);
    vk.destroyShaderModule(device->Handle(), vertex, nullptr);
    vk.destroyShaderModule(device->Handle(), fragment, nullptr);
    if (result != VK_SUCCESS) return Fail("pipeline: " + error);
    vk.destroyPipeline(device->Handle(), pipeline, nullptr);
    std::printf("OK %s: %zu vertex inputs, %zu bindings\n", module->key.c_str(), input->attributes.size(), input->bindings.size());
    return 0;
}

int DumpTestShaders(const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    const std::pair<const char*, std::pair<const std::uint32_t*, std::size_t>> shaders[] = {
        {"test_vertex.spv", {kGraphicsVertex, sizeof(kGraphicsVertex)}},
        {"test_inputs.spv", {kGraphicsInputs, sizeof(kGraphicsInputs)}},
        {"test_float.spv", {kGraphicsFloat, sizeof(kGraphicsFloat)}},
        {"test_uint.spv", {kGraphicsUint, sizeof(kGraphicsUint)}},
    };
    for (const auto& [name, code] : shaders) {
        std::ofstream(folder / name, std::ios::binary).write(reinterpret_cast<const char*>(code.first), static_cast<std::streamsize>(code.second));
    }
    std::puts("dumped");
    return 0;
}

int GraphicsRequests(int argc, char** argv) {
    ShaderStore store;
    if (!LoadStore(argv[2], store)) return 1;
    const std::vector<unsigned char> vertex = ReadBytes(Utf8Path(argv[3]));
    const std::vector<unsigned char> pixel = ReadBytes(Utf8Path(argv[4]));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = DefaultGraphics();
    desc.VS = {vertex.data(), vertex.size()};
    desc.PS = {pixel.data(), pixel.size()};
    if (HasFlag(argc, argv, "--layout")) desc.InputLayout = {kTestLayout, 4};
    std::string error;
    const std::optional<GraphicsRequest> request = GraphicsPipelineRequest(desc, store, error);
    if (!request) {
        std::printf("error\t%s\n", error.c_str());
        return 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC blended = desc;
    blended.BlendState.RenderTarget[0].BlendEnable = TRUE;
    blended.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blended.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    const std::optional<GraphicsRequest> other = GraphicsPipelineRequest(blended, store, error);
    if (!other || other->key == request->key) return Fail("a different blend state must give another key");
    std::printf("request %zu shaders, %zu attributes, key %zu chars\n", request->shaders.size(), request->input.attributes.size(), request->key.size());
    DeviceRequirements requirements;
    const std::unique_ptr<VulkanDevice> device = VulkanDevice::Create(requirements, error);
    if (!device) {
        std::printf("SKIP no usable Vulkan device: %s\n", error.c_str());
        return kNoDevice;
    }
    std::printf("pipeline cache control %s\n", device->PipelineCacheControl() ? "yes" : "no");
    PipelineCache cache(*device, Utf8Path(FindOption(argc, argv, "--cache", "")));
    std::vector<PipelineEntry> entries(7);
    {
        PipelineLibrary library(*device, cache.Handle(), std::strtoull(FindOption(argc, argv, "--limit", "0").c_str(), nullptr, 10));
        std::atomic<std::size_t> next{0};
        std::vector<std::thread> pool;
        for (unsigned worker = 0; worker < 4; ++worker) {
            pool.emplace_back([&] {
                for (std::size_t index = next++; index < entries.size(); index = next++) {
                    entries[index] = library.Graphics(index % 3 == 2 ? *other : *request);
                }
            });
        }
        for (std::thread& thread : pool) thread.join();
        for (std::size_t index = 0; index < entries.size(); ++index) {
            std::printf("%zu\t%s\t%llu\t%.3f\t%s\n", index, PipelineOriginName(entries[index].origin), static_cast<unsigned long long>(entries[index].bytes), entries[index].seconds,
                        entries[index].error.c_str());
        }
        if (entries[0].pipeline != entries[1].pipeline || entries[2].pipeline != entries[5].pipeline || (entries[0].pipeline != VK_NULL_HANDLE && entries[0].pipeline == entries[2].pipeline)) {
            return Fail("requests with one key must share one pipeline, and two keys two pipelines");
        }
        const PipelineLibraryStats stats = library.Stats();
        std::printf("stats ready=%zu compiled=%zu refused=%zu failed=%zu\n", stats.ready, stats.compiled, stats.refused, stats.failed);
    }
    if (HasFlag(argc, argv, "--save")) {
        std::size_t written = 0;
        if (!cache.Save(written, error)) return Fail("cache save: " + error);
    }
    return 0;
}

int StatesSelfTest() {
    std::string error;
    const auto usage = [&error](UINT states, bool depth) { return UsageOfState(static_cast<D3D12_RESOURCE_STATES>(states), depth, error); };
    const std::optional<ResourceUsage> common = usage(D3D12_RESOURCE_STATE_COMMON, false);
    if (!common || common->layout != VK_IMAGE_LAYOUT_GENERAL || common->stages != VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) return Fail("common");
    const std::pair<UINT, VkImageLayout> layouts[] = {
        {D3D12_RESOURCE_STATE_RENDER_TARGET, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {D3D12_RESOURCE_STATE_UNORDERED_ACCESS, VK_IMAGE_LAYOUT_GENERAL},
        {D3D12_RESOURCE_STATE_DEPTH_WRITE, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL},
        {D3D12_RESOURCE_STATE_DEPTH_READ, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
        {D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {D3D12_RESOURCE_STATE_COPY_DEST, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL},
        {D3D12_RESOURCE_STATE_COPY_SOURCE, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
        {D3D12_RESOURCE_STATE_RESOLVE_DEST, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL},
        {D3D12_RESOURCE_STATE_RESOLVE_SOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
        {D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, VK_IMAGE_LAYOUT_GENERAL},
        {D3D12_RESOURCE_STATE_GENERIC_READ, VK_IMAGE_LAYOUT_GENERAL},
        {D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, VK_IMAGE_LAYOUT_GENERAL},
    };
    for (const auto& [states, layout] : layouts) {
        const std::optional<ResourceUsage> found = usage(states, false);
        if (!found || found->layout != layout) return Fail("layout of state " + std::to_string(states) + ": " + error);
    }
    const std::optional<ResourceUsage> depthSampled = usage(D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true);
    if (!depthSampled || depthSampled->layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL ||
        (depthSampled->stages & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) == 0 || (depthSampled->access & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT) == 0) return Fail("sampled depth");
    if (usage(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, true)->layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) return Fail("depth shader resource");
    const std::optional<ResourceUsage> generic = usage(D3D12_RESOURCE_STATE_GENERIC_READ, false);
    const VkAccessFlags2 genericAccess = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
    if ((generic->access & genericAccess) != genericAccess || (generic->access & (VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT)) != 0) return Fail("generic read access");
    const std::pair<UINT, const char*> refusals[] = {
        {D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, "combines a write state with other states"},
        {D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "0x408 combines write states"},
        {D3D12_RESOURCE_STATE_STREAM_OUT, "are not supported"},
        {D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, "0x400000 (stream output, ray tracing, shading rate or video) are not supported"},
        {0x4000, "bits 0x4000 are unknown"},
        {0x80000000u, "bits 0x80000000 are unknown"},
    };
    for (const auto& [states, expected] : refusals) {
        error.clear();
        if (usage(states, false) || error.find(expected) == std::string::npos) return Fail(std::string("refusal '") + expected + "' gave '" + error + "'");
    }
    const std::optional<VkImageMemoryBarrier2> barrier = ImageTransition(D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, false, error);
    if (!barrier || barrier->oldLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || barrier->newLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
        barrier->srcStageMask != VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT || barrier->dstStageMask != VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT ||
        barrier->srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED) return Fail("render target to shader resource");
    if (ImageTransition(D3D12_RESOURCE_STATE_COMMON, static_cast<D3D12_RESOURCE_STATES>(0x4000), false, error)) return Fail("transition to an unknown state");
    const std::optional<VkMemoryBarrier2> buffer = BufferTransition(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, error);
    if (!buffer || buffer->srcAccessMask != VK_ACCESS_2_TRANSFER_WRITE_BIT || (buffer->dstAccessMask & VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT) == 0) return Fail("buffer transition");
    if (UnorderedAccessBarrier().srcAccessMask != VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT || AliasingBarrier().srcStageMask != VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) return Fail("uav and aliasing");
    std::puts("PASS states selftest");
    return 0;
}

int StatesGpu(bool validate) {
    DeviceRequirements requirements;
    requirements.validation = validate;
    std::string error;
    const std::unique_ptr<VulkanDevice> device = VulkanDevice::Create(requirements, error);
    if (!device) {
        std::printf("SKIP no usable Vulkan device: %s\n", error.c_str());
        return validate && error == "the Khronos validation layer is not installed" ? kNoValidation : kNoDevice;
    }
    const DeviceFunctions& vk = device->Vk();
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };
    const auto makeImage = [&](VkFormat format, VkImageUsageFlags usage, Image& out) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {16, 16, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        if (vk.createImage(device->Handle(), &info, nullptr, &out.image) != VK_SUCCESS) return false;
        VkMemoryRequirements needs{};
        vk.getImageMemoryRequirements(device->Handle(), out.image, &needs);
        const std::optional<std::uint32_t> type = device->MemoryType(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = type ? *type : 0;
        return type && vk.allocateMemory(device->Handle(), &allocate, nullptr, &out.memory) == VK_SUCCESS && vk.bindImageMemory(device->Handle(), out.image, out.memory, 0) == VK_SUCCESS;
    };
    Image color;
    Image depth;
    const VkImageUsageFlags colorUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags depthUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!makeImage(VK_FORMAT_R8G8B8A8_UNORM, colorUsage, color) || !makeImage(VK_FORMAT_D32_SFLOAT, depthUsage, depth)) return Fail("test images");
    const UINT colorStates[] = {
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COMMON,
    };
    const UINT depthStates[] = {
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON,
    };
    const UINT bufferStates[] = {
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COMMON,
    };
    std::size_t recorded = 0;
    bool failed = false;
    const bool submitted = device->Submit(
        [&](VkCommandBuffer commands) {
            const auto walk = [&](const Image& image, VkImageAspectFlags aspect, bool isDepth, const UINT* states, std::size_t count) {
                VkImageMemoryBarrier2 start{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                start.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                start.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                start.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                start.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                start.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                start.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                start.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                start.image = image.image;
                start.subresourceRange = {aspect, 0, 1, 0, 1};
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.imageMemoryBarrierCount = 1;
                dependency.pImageMemoryBarriers = &start;
                vk.cmdPipelineBarrier2(commands, &dependency);
                UINT current = D3D12_RESOURCE_STATE_COMMON;
                for (std::size_t index = 0; index < count; ++index) {
                    std::optional<VkImageMemoryBarrier2> barrier = ImageTransition(static_cast<D3D12_RESOURCE_STATES>(current), static_cast<D3D12_RESOURCE_STATES>(states[index]), isDepth, error);
                    if (!barrier) {
                        failed = true;
                        return;
                    }
                    barrier->image = image.image;
                    barrier->subresourceRange = {aspect, 0, 1, 0, 1};
                    dependency.pImageMemoryBarriers = &*barrier;
                    vk.cmdPipelineBarrier2(commands, &dependency);
                    if (states[index] == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                        const VkMemoryBarrier2 uav = UnorderedAccessBarrier();
                        VkDependencyInfo memory{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                        memory.memoryBarrierCount = 1;
                        memory.pMemoryBarriers = &uav;
                        vk.cmdPipelineBarrier2(commands, &memory);
                        ++recorded;
                    }
                    current = states[index];
                    ++recorded;
                }
            };
            walk(color, VK_IMAGE_ASPECT_COLOR_BIT, false, colorStates, std::size(colorStates));
            walk(depth, VK_IMAGE_ASPECT_DEPTH_BIT, true, depthStates, std::size(depthStates));
            UINT current = D3D12_RESOURCE_STATE_COMMON;
            for (const UINT state : bufferStates) {
                const std::optional<VkMemoryBarrier2> barrier = BufferTransition(static_cast<D3D12_RESOURCE_STATES>(current), static_cast<D3D12_RESOURCE_STATES>(state), error);
                if (!barrier) {
                    failed = true;
                    return;
                }
                VkDependencyInfo memory{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                memory.memoryBarrierCount = 1;
                memory.pMemoryBarriers = &*barrier;
                vk.cmdPipelineBarrier2(commands, &memory);
                current = state;
                ++recorded;
            }
            const VkMemoryBarrier2 aliasing = AliasingBarrier();
            VkDependencyInfo memory{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            memory.memoryBarrierCount = 1;
            memory.pMemoryBarriers = &aliasing;
            vk.cmdPipelineBarrier2(commands, &memory);
            ++recorded;
        },
        error);
    for (const Image* image : {&color, &depth}) {
        vk.destroyImage(device->Handle(), image->image, nullptr);
        vk.freeMemory(device->Handle(), image->memory, nullptr);
    }
    if (!submitted || failed) return Fail("barriers: " + error);
    std::printf("barriers recorded and executed: %zu\n", recorded);
    if (validate) {
        std::printf("validation errors=%zu\n", device->ValidationErrors());
        for (const std::string& message : device->ValidationMessages()) std::printf("  %.600s\n", message.c_str());
        if (device->ValidationErrors() != 0) return 1;
    }
    std::puts("PASS states gpu");
    return 0;
}

D3D12_RESOURCE_DESC TextureDesc2D(DXGI_FORMAT format, UINT64 width, UINT height, UINT16 layers, UINT16 mips, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = layers;
    desc.MipLevels = mips;
    desc.Format = format;
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return desc;
}

struct TextureCase {
    const char* name;
    D3D12_RESOURCE_DESC desc;
};

std::vector<TextureCase> TextureCases() {
    std::vector<TextureCase> cases;
    cases.push_back({"rgba8 render target and UAV, full chain", TextureDesc2D(DXGI_FORMAT_R8G8B8A8_UNORM, 256, 128, 1, 0,
                                                                                static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))});
    cases.push_back({"typeless rgba8 render target", TextureDesc2D(DXGI_FORMAT_R8G8B8A8_TYPELESS, 64, 64, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)});
    cases.push_back({"d32 depth, sampled", TextureDesc2D(DXGI_FORMAT_D32_FLOAT, 512, 512, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)});
    cases.push_back({"r32 typeless depth", TextureDesc2D(DXGI_FORMAT_R32_TYPELESS, 512, 512, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)});
    cases.push_back({"d32s8 typeless depth, not sampled", TextureDesc2D(DXGI_FORMAT_R32G8X24_TYPELESS, 128, 128, 2, 1,
                                                                        static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))});
    cases.push_back({"bc7 cube", TextureDesc2D(DXGI_FORMAT_BC7_UNORM_SRGB, 128, 128, 6, 0, D3D12_RESOURCE_FLAG_NONE)});
    cases.push_back({"bc1 typeless array", TextureDesc2D(DXGI_FORMAT_BC1_TYPELESS, 64, 32, 4, 3, D3D12_RESOURCE_FLAG_NONE)});
    cases.push_back({"r32 typeless color UAV", TextureDesc2D(DXGI_FORMAT_R32_TYPELESS, 64, 64, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)});
    D3D12_RESOURCE_DESC msaa = TextureDesc2D(DXGI_FORMAT_R16G16B16A16_FLOAT, 320, 180, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    msaa.SampleDesc = {4, 0};
    cases.push_back({"4x rgba16f render target", msaa});
    D3D12_RESOURCE_DESC volume = TextureDesc2D(DXGI_FORMAT_R16G16B16A16_FLOAT, 32, 32, 16, 0,
                                               static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET));
    volume.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    cases.push_back({"3d rgba16f UAV and render target", volume});
    D3D12_RESOURCE_DESC line = TextureDesc2D(DXGI_FORMAT_R32_FLOAT, 1024, 1, 8, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    line.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    cases.push_back({"1d r32f array UAV", line});
    cases.push_back({"bgrx8 sampled", TextureDesc2D(DXGI_FORMAT_B8G8R8X8_UNORM, 64, 64, 1, 1, D3D12_RESOURCE_FLAG_NONE)});
    return cases;
}

int TexturesSelfTest() {
    std::string error;
    if (FullMipChain(256, 128, 1) != 9 || FullMipChain(1, 1, 1) != 1 || FullMipChain(1000, 1, 1) != 10 || FullMipChain(4, 4, 64) != 7) return Fail("full mip chain");
    const std::optional<TextureFormat> depth = TextureStorageFormat(DXGI_FORMAT_R24G8_TYPELESS, true);
    if (!depth || depth->format != VK_FORMAT_D24_UNORM_S8_UINT || depth->aspects != (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) || depth->mutableFormat) return Fail("r24g8 typeless");
    if (TextureStorageFormat(DXGI_FORMAT_R32_TYPELESS, true)->format != VK_FORMAT_D32_SFLOAT || TextureStorageFormat(DXGI_FORMAT_R32_TYPELESS, false)->format != VK_FORMAT_R32_UINT ||
        !TextureStorageFormat(DXGI_FORMAT_R32_TYPELESS, false)->mutableFormat) return Fail("r32 typeless");
    if (TextureStorageFormat(DXGI_FORMAT_BC1_UNORM, false)->format != VK_FORMAT_BC1_RGBA_UNORM_BLOCK || !TextureStorageFormat(DXGI_FORMAT_BC6H_SF16, false)->compressed ||
        TextureStorageFormat(DXGI_FORMAT_BC6H_TYPELESS, false)->format != VK_FORMAT_BC6H_UFLOAT_BLOCK) return Fail("block formats");
    if (TextureStorageFormat(DXGI_FORMAT_D16_UNORM, true)->aspects != VK_IMAGE_ASPECT_DEPTH_BIT || TextureStorageFormat(DXGI_FORMAT_R8G8B8A8_UNORM, false)->mutableFormat) return Fail("direct formats");
    if (TextureStorageFormat(static_cast<DXGI_FORMAT>(200), false) || TextureStorageFormat(DXGI_FORMAT_UNKNOWN, false)) return Fail("unknown formats");
    const std::vector<TextureCase> cases = TextureCases();
    for (const TextureCase& test : cases) {
        if (!TranslateTextureDesc(test.desc, error)) return Fail(std::string(test.name) + ": " + error);
    }
    const std::optional<TextureImage> chain = TranslateTextureDesc(cases[0].desc, error);
    const VkImageUsageFlags rgbaUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    if (chain->info.mipLevels != 9 || chain->info.usage != rgbaUsage || chain->info.flags != 0 || chain->info.imageType != VK_IMAGE_TYPE_2D) return Fail("rgba8 image");
    const std::optional<TextureImage> typeless = TranslateTextureDesc(cases[1].desc, error);
    if ((typeless->info.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) == 0 || (typeless->info.flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) == 0) return Fail("typeless is mutable");
    const std::optional<TextureImage> denied = TranslateTextureDesc(cases[4].desc, error);
    if ((denied->info.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0 || (denied->info.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0 || denied->info.arrayLayers != 2) return Fail("denied shader resource");
    const std::optional<TextureImage> cube = TranslateTextureDesc(cases[5].desc, error);
    if ((cube->info.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) == 0 || cube->info.mipLevels != 8 || cube->features != std::set<std::string>{"textureCompressionBC"}) return Fail("bc7 cube");
    const std::optional<TextureImage> volume = TranslateTextureDesc(cases[9].desc, error);
    if (volume->info.imageType != VK_IMAGE_TYPE_3D || volume->info.extent.depth != 16 || volume->info.arrayLayers != 1 || volume->info.mipLevels != 6 ||
        (volume->info.flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT) == 0) return Fail("volume");
    if (TranslateTextureDesc(cases[8].desc, error)->info.samples != VK_SAMPLE_COUNT_4_BIT) return Fail("msaa samples");

    const std::pair<void (*)(D3D12_RESOURCE_DESC&), const char*> refusals[] = {
        {[](D3D12_RESOURCE_DESC& d) { d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; }, "resource dimension 1 is not a texture"},
        {[](D3D12_RESOURCE_DESC& d) { d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; }, "texture layout 1 is not supported"},
        {[](D3D12_RESOURCE_DESC& d) { d.Format = static_cast<DXGI_FORMAT>(200); }, "texture format 200 has no Vulkan mapping"},
        {[](D3D12_RESOURCE_DESC& d) { d.MipLevels = 12; }, "12 mip levels are more than the 9 of a full chain"},
        {[](D3D12_RESOURCE_DESC& d) { d.SampleDesc.Count = 3; }, "sample count 3"},
        {[](D3D12_RESOURCE_DESC& d) { d.SampleDesc.Count = 4; }, "multisampled texture must be 2D with one mip level and no unordered access"},
        {[](D3D12_RESOURCE_DESC& d) { d.Width = 0; }, "zero or oversized extent"},
        {[](D3D12_RESOURCE_DESC& d) { d.Width = 0x100000000ull; }, "zero or oversized extent"},
        {[](D3D12_RESOURCE_DESC& d) { d.Format = DXGI_FORMAT_D32_FLOAT; }, "depth format 40 without ALLOW_DEPTH_STENCIL"},
        {[](D3D12_RESOURCE_DESC& d) { d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL; }, "a depth-stencil texture has the color format 28"},
        {[](D3D12_RESOURCE_DESC& d) { d.Format = DXGI_FORMAT_D32_FLOAT; d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS); }, "cannot also allow"},
        {[](D3D12_RESOURCE_DESC& d) { d.Flags = D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE; }, "DENY_SHADER_RESOURCE without ALLOW_DEPTH_STENCIL"},
        {[](D3D12_RESOURCE_DESC& d) { d.Format = DXGI_FORMAT_BC3_UNORM; }, "a block-compressed texture is 1D or allows"},
        {[](D3D12_RESOURCE_DESC& d) { d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x100); }, "resource flags 256 are not supported"},
        {[](D3D12_RESOURCE_DESC& d) { d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D; }, "a 1D texture has height 128"},
    };
    for (const auto& [edit, expected] : refusals) {
        D3D12_RESOURCE_DESC desc = cases[0].desc;
        edit(desc);
        error.clear();
        if (TranslateTextureDesc(desc, error) || error.find(expected) == std::string::npos) return Fail(std::string("refusal '") + expected + "' gave '" + error + "'");
    }
    std::puts("PASS textures selftest");
    return 0;
}

int TexturesGpu(bool validate) {
    DeviceRequirements requirements;
    requirements.validation = validate;
    std::string error;
    const std::unique_ptr<VulkanDevice> device = VulkanDevice::Create(requirements, error);
    if (!device) {
        std::printf("SKIP no usable Vulkan device: %s\n", error.c_str());
        return validate && error == "the Khronos validation layer is not installed" ? kNoValidation : kNoDevice;
    }
    std::size_t created = 0;
    std::size_t skipped = 0;
    int status = 0;
    for (const TextureCase& test : TextureCases()) {
        const std::optional<TextureImage> image = TranslateTextureDesc(test.desc, error);
        if (!image) {
            status = Fail(std::string(test.name) + ": " + error);
            continue;
        }
        std::string missing;
        for (const std::string& feature : image->features) {
            if (!device->HasFeature(feature)) missing += feature;
        }
        if (!missing.empty()) {
            std::printf("SKIPPED %s: the device lacks %s\n", test.name, missing.c_str());
            ++skipped;
            continue;
        }
        VkImage handle = VK_NULL_HANDLE;
        const VkResult result = device->Vk().createImage(device->Handle(), &image->info, nullptr, &handle);
        if (result != VK_SUCCESS) {
            status = Fail(std::string(test.name) + ": vkCreateImage failed with " + std::to_string(static_cast<int>(result)));
            continue;
        }
        device->Vk().destroyImage(device->Handle(), handle, nullptr);
        std::printf("OK %s\n", test.name);
        ++created;
    }
    std::printf("textures: %zu created, %zu skipped for missing features\n", created, skipped);
    if (validate) {
        std::printf("validation errors=%zu\n", device->ValidationErrors());
        for (const std::string& message : device->ValidationMessages()) std::printf("  %.600s\n", message.c_str());
        if (device->ValidationErrors() != 0) status = 1;
    }
    if (status == 0) std::puts("PASS textures gpu");
    return status;
}

D3D12_RESOURCE_DESC ViewTexture(DXGI_FORMAT format, UINT16 layers, UINT16 mips, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
    D3D12_RESOURCE_DESC desc = TextureDesc2D(format, 64, dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1 : 64, layers, mips, flags);
    desc.Dimension = dimension;
    return desc;
}

struct ViewCase {
    const char* name;
    D3D12_RESOURCE_DESC resource;
    char kind;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav;
    D3D12_RENDER_TARGET_VIEW_DESC rtv;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv;
    bool useDefault;
};

std::vector<ViewCase> ViewCases() {
    const D3D12_RESOURCE_FLAGS rtUav = static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC colorArray = ViewTexture(DXGI_FORMAT_R8G8B8A8_TYPELESS, 4, 0, rtUav);
    const D3D12_RESOURCE_DESC depth = ViewTexture(DXGI_FORMAT_R32G8X24_TYPELESS, 2, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    const D3D12_RESOURCE_DESC cubes = ViewTexture(DXGI_FORMAT_BC7_UNORM, 12, 0, D3D12_RESOURCE_FLAG_NONE);
    D3D12_RESOURCE_DESC volume = ViewTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, 16, 0, rtUav, D3D12_RESOURCE_DIMENSION_TEXTURE3D);
    const D3D12_RESOURCE_DESC bgrx = ViewTexture(DXGI_FORMAT_B8G8R8X8_UNORM, 1, 1, D3D12_RESOURCE_FLAG_NONE);
    const D3D12_RESOURCE_DESC line = ViewTexture(DXGI_FORMAT_R32_FLOAT, 3, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_DIMENSION_TEXTURE1D);
    D3D12_RESOURCE_DESC msaa = ViewTexture(DXGI_FORMAT_R8G8B8A8_UNORM, 2, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    msaa.SampleDesc = {4, 0};
    std::vector<ViewCase> cases;
    const auto add = [&cases](const char* name, const D3D12_RESOURCE_DESC& resource, char kind) -> ViewCase& {
        cases.push_back(ViewCase{name, resource, kind, {}, {}, {}, {}, false});
        return cases.back();
    };
    ViewCase& srgb = add("srgb 2D array view of mips 1-2", colorArray, 's');
    srgb.srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    srgb.srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srgb.srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srgb.srv.Texture2DArray = {1, 2, 1, 3, 0, 0.0f};
    ViewCase& swizzle = add("uint 2D view with swizzle", colorArray, 's');
    swizzle.srv.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    swizzle.srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    swizzle.srv.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(2, 1, 0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
    swizzle.srv.Texture2D = {0, static_cast<UINT>(-1), 0, 0.0f};
    add("uav of mip 3, array", colorArray, 'u').uav = {DXGI_FORMAT_R8G8B8A8_UINT, D3D12_UAV_DIMENSION_TEXTURE2DARRAY, {}};
    cases.back().uav.Texture2DArray = {3, 0, 4, 0};
    add("rtv of layer 2", colorArray, 'r').rtv = {DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RTV_DIMENSION_TEXTURE2DARRAY, {}};
    cases.back().rtv.Texture2DArray = {0, 2, 1, 0};
    ViewCase& depthRead = add("depth plane of d32s8", depth, 's');
    depthRead.srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    depthRead.srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    depthRead.srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthRead.srv.Texture2DArray = {0, 1, 0, 2, 0, 0.0f};
    ViewCase& stencilRead = add("stencil plane of d32s8", depth, 's');
    stencilRead.srv.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    stencilRead.srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    stencilRead.srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    stencilRead.srv.Texture2D = {0, 1, 1, 0.0f};
    add("dsv of layer 1", depth, 'd').dsv = {DXGI_FORMAT_D32_FLOAT_S8X24_UINT, D3D12_DSV_DIMENSION_TEXTURE2DARRAY, D3D12_DSV_FLAG_READ_ONLY_STENCIL, {}};
    cases.back().dsv.Texture2DArray = {0, 1, 1};
    ViewCase& cube = add("cube array of two", cubes, 's');
    cube.srv.Format = DXGI_FORMAT_BC7_UNORM;
    cube.srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
    cube.srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    cube.srv.TextureCubeArray = {0, static_cast<UINT>(-1), 0, 2, 0.0f};
    ViewCase& oneCube = add("cube", cubes, 's');
    oneCube.srv = cube.srv;
    oneCube.srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    oneCube.srv.TextureCube = {1, 2, 0.0f};
    add("default srv of a volume", volume, 's').useDefault = true;
    add("default uav of a volume", volume, 'u').useDefault = true;
    add("rtv of volume slices 4-11", volume, 'r').rtv = {DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RTV_DIMENSION_TEXTURE3D, {}};
    cases.back().rtv.Texture3D = {0, 4, 8};
    add("default srv of bgrx", bgrx, 's').useDefault = true;
    add("default uav of a 1D array", line, 'u').useDefault = true;
    add("default srv of a 1D array", line, 's').useDefault = true;
    add("default srv of msaa array", msaa, 's').useDefault = true;
    add("default rtv of msaa array", msaa, 'r').useDefault = true;
    return cases;
}

std::optional<ImageView> MakeView(const ViewCase& test, std::string& error) {
    switch (test.kind) {
    case 's': return ShaderResourceView(test.resource, test.useDefault ? nullptr : &test.srv, error);
    case 'u': return UnorderedAccessView(test.resource, test.useDefault ? nullptr : &test.uav, error);
    case 'r': return RenderTargetView(test.resource, test.useDefault ? nullptr : &test.rtv, error);
    default: return DepthStencilView(test.resource, test.useDefault ? nullptr : &test.dsv, error);
    }
}

std::vector<D3D12_SAMPLER_DESC> SamplerCases() {
    std::vector<D3D12_SAMPLER_DESC> cases;
    D3D12_SAMPLER_DESC base{D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0.0f, 1,
                            D3D12_COMPARISON_FUNC_NEVER, {0, 0, 0, 0}, 0.0f, D3D12_FLOAT32_MAX};
    cases.push_back(base);
    D3D12_SAMPLER_DESC aniso = base;
    aniso.Filter = D3D12_FILTER_ANISOTROPIC;
    aniso.MaxAnisotropy = 16;
    aniso.MipLODBias = -0.5f;
    cases.push_back(aniso);
    D3D12_SAMPLER_DESC shadow = base;
    shadow.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadow.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadow.AddressU = shadow.AddressV = shadow.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadow.BorderColor[0] = shadow.BorderColor[1] = shadow.BorderColor[2] = shadow.BorderColor[3] = 1.0f;
    cases.push_back(shadow);
    D3D12_SAMPLER_DESC point = base;
    point.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    point.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    point.AddressV = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    point.AddressW = D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    point.MinLOD = 1.0f;
    point.MaxLOD = 4.0f;
    cases.push_back(point);
    D3D12_SAMPLER_DESC maximum = base;
    maximum.Filter = D3D12_FILTER_MAXIMUM_MIN_MAG_MIP_LINEAR;
    maximum.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    maximum.BorderColor[3] = 1.0f;
    cases.push_back(maximum);
    return cases;
}

int ViewsSelfTest() {
    std::string error;
    const std::optional<VkComponentMapping> identity = ComponentMapping(D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, false);
    if (!identity || identity->r != VK_COMPONENT_SWIZZLE_R || identity->a != VK_COMPONENT_SWIZZLE_A) return Fail("identity mapping");
    const std::optional<VkComponentMapping> forced = ComponentMapping(D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(3, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, 0, 3), true);
    if (!forced || forced->r != VK_COMPONENT_SWIZZLE_ONE || forced->g != VK_COMPONENT_SWIZZLE_ZERO || forced->b != VK_COMPONENT_SWIZZLE_R || forced->a != VK_COMPONENT_SWIZZLE_ONE) return Fail("forced mapping");
    if (ComponentMapping(0x688, false) || ComponentMapping(D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING | 0x2000, false) ||
        ComponentMapping(D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(6, 1, 2, 3), false)) return Fail("invalid mappings");
    const std::vector<ViewCase> cases = ViewCases();
    for (const ViewCase& test : cases) {
        if (!MakeView(test, error)) return Fail(std::string(test.name) + ": " + error);
    }
    const auto view = [&](std::size_t index) { return *MakeView(cases[index], error); };
    const ImageView srgb = view(0);
    if (srgb.info.format != VK_FORMAT_R8G8B8A8_SRGB || srgb.info.viewType != VK_IMAGE_VIEW_TYPE_2D_ARRAY || srgb.info.subresourceRange.baseMipLevel != 1 ||
        srgb.info.subresourceRange.levelCount != 2 || srgb.info.subresourceRange.baseArrayLayer != 1 || srgb.info.subresourceRange.layerCount != 3) return Fail("srgb view");
    const ImageView swizzled = view(1);
    if (swizzled.info.components.r != VK_COMPONENT_SWIZZLE_B || swizzled.info.components.a != VK_COMPONENT_SWIZZLE_ONE || swizzled.info.subresourceRange.levelCount != 7) return Fail("swizzle view");
    if (view(4).info.subresourceRange.aspectMask != VK_IMAGE_ASPECT_DEPTH_BIT || view(4).info.format != VK_FORMAT_D32_SFLOAT_S8_UINT) return Fail("depth plane");
    if (view(5).info.subresourceRange.aspectMask != VK_IMAGE_ASPECT_STENCIL_BIT) return Fail("stencil plane");
    if (view(6).info.subresourceRange.aspectMask != (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) || view(6).info.subresourceRange.baseArrayLayer != 1) return Fail("dsv");
    const ImageView cubeArray = view(7);
    if (cubeArray.info.viewType != VK_IMAGE_VIEW_TYPE_CUBE_ARRAY || cubeArray.info.subresourceRange.layerCount != 12 || !cubeArray.features.count("imageCubeArray")) return Fail("cube array");
    if (view(8).info.subresourceRange.layerCount != 6 || view(8).info.subresourceRange.baseMipLevel != 1) return Fail("cube");
    if (view(9).info.viewType != VK_IMAGE_VIEW_TYPE_3D || view(9).info.subresourceRange.levelCount != 7) return Fail("volume srv");
    const ImageView slices = view(11);
    if (slices.info.viewType != VK_IMAGE_VIEW_TYPE_2D_ARRAY || slices.info.subresourceRange.baseArrayLayer != 4 || slices.info.subresourceRange.layerCount != 8) return Fail("volume slices");
    if (view(12).info.components.a != VK_COMPONENT_SWIZZLE_ONE || view(12).info.format != VK_FORMAT_B8G8R8A8_UNORM) return Fail("bgrx alpha");
    if (view(13).info.viewType != VK_IMAGE_VIEW_TYPE_1D_ARRAY || view(13).info.subresourceRange.layerCount != 3) return Fail("1D array");
    if (view(15).info.viewType != VK_IMAGE_VIEW_TYPE_2D_ARRAY || view(16).info.viewType != VK_IMAGE_VIEW_TYPE_2D_ARRAY) return Fail("msaa arrays");

    struct Refusal {
        std::size_t base;
        void (*edit)(ViewCase&);
        const char* expected;
    };
    const Refusal refusals[] = {
        {0, [](ViewCase& v) { v.srv.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; }, "is typeless"},
        {0, [](ViewCase& v) { v.srv.Format = DXGI_FORMAT_R32_FLOAT; }, "view format 41 is not in the format family of the texture's 27"},
        {0, [](ViewCase& v) { v.srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; }, "is not in the format family"},
        {12, [](ViewCase& v) { v.useDefault = false; v.srv = {DXGI_FORMAT_B8G8R8X8_UNORM_SRGB, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, {}}; v.srv.Texture2D = {0, 1, 0, 0.0f}; }, "differs from a texture that is not typeless"},
        {0, [](ViewCase& v) { v.srv.Texture2DArray.MostDetailedMip = 7; }, "mip range 7+2"},
        {0, [](ViewCase& v) { v.srv.Texture2DArray.ArraySize = 4; }, "array range 1+4"},
        {0, [](ViewCase& v) { v.srv.Texture2DArray.ResourceMinLODClamp = 1.0f; }, "minimum LOD clamp"},
        {0, [](ViewCase& v) { v.srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; }, "does not match the texture's dimension"},
        {0, [](ViewCase& v) { v.srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; }, "dimension 1 is not a texture view"},
        {0, [](ViewCase& v) { v.srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE; }, "not a square array of six"},
        {0, [](ViewCase& v) { v.srv.Shader4ComponentMapping = 0; }, "component mapping 0"},
        {5, [](ViewCase& v) { v.srv.Texture2D.PlaneSlice = 0; }, "plane slice 0 does not match"},
        {4, [](ViewCase& v) { v.srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; }, "does not read a depth-stencil texture"},
        {6, [](ViewCase& v) { v.dsv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; }, "is not a depth format"},
        {6, [](ViewCase& v) { v.dsv.Flags = static_cast<D3D12_DSV_FLAGS>(4); }, "flags 4 are unknown"},
        {7, [](ViewCase& v) { v.srv.TextureCubeArray.NumCubes = 3; }, "do not fit"},
        {10, [](ViewCase& v) { v.useDefault = false; v.uav = {DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_UAV_DIMENSION_TEXTURE3D, {}}; v.uav.Texture3D = {0, 2, 4}; }, "part of a 3D texture's depth"},
        {11, [](ViewCase& v) { v.rtv.Texture3D = {0, 10, 8}; }, "array range 10+8"},
        {12, [](ViewCase& v) { v.kind = 'u'; }, "without ALLOW_UNORDERED_ACCESS"},
        {12, [](ViewCase& v) { v.kind = 'r'; }, "without ALLOW_RENDER_TARGET"},
        {12, [](ViewCase& v) { v.kind = 'd'; }, "without ALLOW_DEPTH_STENCIL"},
        {2, [](ViewCase& v) { v.uav.ViewDimension = static_cast<D3D12_UAV_DIMENSION>(9); }, "dimension 9 is not a supported texture view"},
    };
    for (const Refusal& refusal : refusals) {
        ViewCase test = cases[refusal.base];
        refusal.edit(test);
        error.clear();
        if (MakeView(test, error) || error.find(refusal.expected) == std::string::npos) return Fail(std::string("refusal '") + refusal.expected + "' gave '" + error + "'");
    }
    D3D12_RESOURCE_DESC denied = ViewTexture(DXGI_FORMAT_D32_FLOAT, 1, 1, static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE));
    if (ShaderResourceView(denied, nullptr, error) || error.find("denies shader resources") == std::string::npos) return Fail("denied srv");

    const std::vector<D3D12_SAMPLER_DESC> samplers = SamplerCases();
    std::vector<Sampler> translated;
    for (const D3D12_SAMPLER_DESC& desc : samplers) {
        std::optional<Sampler> sampler = TranslateSampler(desc, error);
        if (!sampler) return Fail("sampler: " + error);
        translated.push_back(*sampler);
    }
    if (translated[0].info.minFilter != VK_FILTER_LINEAR || translated[0].info.mipmapMode != VK_SAMPLER_MIPMAP_MODE_LINEAR || translated[0].info.maxLod != VK_LOD_CLAMP_NONE ||
        translated[0].info.anisotropyEnable) return Fail("linear sampler");
    if (!translated[1].info.anisotropyEnable || translated[1].info.maxAnisotropy != 16.0f || translated[1].info.mipLodBias != -0.5f || !translated[1].features.count("samplerAnisotropy")) return Fail("anisotropic");
    if (!translated[2].info.compareEnable || translated[2].info.compareOp != VK_COMPARE_OP_LESS_OR_EQUAL || translated[2].info.borderColor != VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE ||
        translated[2].info.mipmapMode != VK_SAMPLER_MIPMAP_MODE_NEAREST || translated[2].info.addressModeU != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) return Fail("shadow sampler");
    if (translated[3].info.minFilter != VK_FILTER_NEAREST || translated[3].info.addressModeV != VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT ||
        translated[3].info.addressModeW != VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE || translated[3].info.minLod != 1.0f || translated[3].info.maxLod != 4.0f) return Fail("point sampler");
    if (!translated[4].usesReduction || translated[4].reduction.reductionMode != VK_SAMPLER_REDUCTION_MODE_MAX || translated[4].info.borderColor != VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK) return Fail("max sampler");
    const std::pair<void (*)(D3D12_SAMPLER_DESC&), const char*> samplerRefusals[] = {
        {[](D3D12_SAMPLER_DESC& d) { d.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER; d.BorderColor[0] = 0.5f; }, "border color other than"},
        {[](D3D12_SAMPLER_DESC& d) { d.Filter = D3D12_FILTER_ANISOTROPIC; d.MaxAnisotropy = 17; }, "anisotropy 17"},
        {[](D3D12_SAMPLER_DESC& d) { d.Filter = static_cast<D3D12_FILTER>(0x2); }, "filter 2 is unknown"},
        {[](D3D12_SAMPLER_DESC& d) { d.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(9); }, "address mode 9"},
        {[](D3D12_SAMPLER_DESC& d) { d.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT; d.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(0); }, "comparison 0"},
        {[](D3D12_SAMPLER_DESC& d) { d.MinLOD = 5.0f; d.MaxLOD = 1.0f; }, "LOD bias or range"},
    };
    for (const auto& [edit, expected] : samplerRefusals) {
        D3D12_SAMPLER_DESC desc = samplers[0];
        edit(desc);
        error.clear();
        if (TranslateSampler(desc, error) || error.find(expected) == std::string::npos) return Fail(std::string("sampler refusal '") + expected + "' gave '" + error + "'");
    }
    std::puts("PASS views selftest");
    return 0;
}

int ViewsGpu(bool validate) {
    DeviceRequirements requirements;
    requirements.validation = validate;
    std::string error;
    const std::unique_ptr<VulkanDevice> device = VulkanDevice::Create(requirements, error);
    if (!device) {
        std::printf("SKIP no usable Vulkan device: %s\n", error.c_str());
        return validate && error == "the Khronos validation layer is not installed" ? kNoValidation : kNoDevice;
    }
    const DeviceFunctions& vk = device->Vk();
    std::size_t created = 0;
    std::size_t skipped = 0;
    int status = 0;
    for (const ViewCase& test : ViewCases()) {
        const std::optional<TextureImage> image = TranslateTextureDesc(test.resource, error);
        const std::optional<ImageView> view = image ? MakeView(test, error) : std::nullopt;
        if (!image || !view) {
            status = Fail(std::string(test.name) + ": " + error);
            continue;
        }
        std::string missing;
        for (const std::string& feature : image->features) {
            if (!device->HasFeature(feature)) missing += feature + " ";
        }
        for (const std::string& feature : view->features) {
            if (!device->HasFeature(feature)) missing += feature + " ";
        }
        if (!missing.empty()) {
            std::printf("SKIPPED %s: the device lacks %s\n", test.name, missing.c_str());
            ++skipped;
            continue;
        }
        VkImage handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView viewHandle = VK_NULL_HANDLE;
        bool ok = vk.createImage(device->Handle(), &image->info, nullptr, &handle) == VK_SUCCESS;
        if (ok) {
            VkMemoryRequirements needs{};
            vk.getImageMemoryRequirements(device->Handle(), handle, &needs);
            const std::optional<std::uint32_t> type = device->MemoryType(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocate.allocationSize = needs.size;
            allocate.memoryTypeIndex = type ? *type : 0;
            ok = type && vk.allocateMemory(device->Handle(), &allocate, nullptr, &memory) == VK_SUCCESS && vk.bindImageMemory(device->Handle(), handle, memory, 0) == VK_SUCCESS;
        }
        if (ok) {
            VkImageViewCreateInfo info = view->info;
            info.image = handle;
            VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
            usage.usage = test.kind == 's' ? VK_IMAGE_USAGE_SAMPLED_BIT : test.kind == 'u' ? VK_IMAGE_USAGE_STORAGE_BIT : test.kind == 'r' ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            info.pNext = &usage;
            ok = vk.createImageView(device->Handle(), &info, nullptr, &viewHandle) == VK_SUCCESS;
        }
        if (viewHandle != VK_NULL_HANDLE) vk.destroyImageView(device->Handle(), viewHandle, nullptr);
        if (handle != VK_NULL_HANDLE) vk.destroyImage(device->Handle(), handle, nullptr);
        if (memory != VK_NULL_HANDLE) vk.freeMemory(device->Handle(), memory, nullptr);
        if (!ok) {
            status = Fail(std::string(test.name) + ": creating the image or view failed");
            continue;
        }
        std::printf("OK view %s\n", test.name);
        ++created;
    }
    for (const D3D12_SAMPLER_DESC& desc : SamplerCases()) {
        std::optional<Sampler> sampler = TranslateSampler(desc, error);
        if (!sampler) {
            status = Fail("sampler: " + error);
            continue;
        }
        std::string missing;
        for (const std::string& feature : sampler->features) {
            if (!device->HasFeature(feature)) missing += feature + " ";
        }
        if (!missing.empty()) {
            std::printf("SKIPPED sampler: the device lacks %s\n", missing.c_str());
            ++skipped;
            continue;
        }
        if (sampler->usesReduction) sampler->info.pNext = &sampler->reduction;
        VkSampler handle = VK_NULL_HANDLE;
        if (vk.createSampler(device->Handle(), &sampler->info, nullptr, &handle) != VK_SUCCESS) {
            status = Fail("vkCreateSampler failed");
            continue;
        }
        vk.destroySampler(device->Handle(), handle, nullptr);
        ++created;
    }
    std::printf("views and samplers: %zu created, %zu skipped for missing features\n", created, skipped);
    if (validate) {
        std::printf("validation errors=%zu\n", device->ValidationErrors());
        for (const std::string& message : device->ValidationMessages()) std::printf("  %.600s\n", message.c_str());
        if (device->ValidationErrors() != 0) status = 1;
    }
    if (status == 0) std::puts("PASS views gpu");
    return status;
}

int CommandsSelfTest() {
    std::string error;
    const std::optional<CopyBlock> bc1 = CopyBlockOf(DXGI_FORMAT_BC1_UNORM);
    if (!bc1 || bc1->width != 4 || bc1->height != 4 || bc1->bytes != 8 || CopyBlockOf(DXGI_FORMAT_BC7_TYPELESS)->bytes != 16) return Fail("block formats");
    if (CopyBlockOf(DXGI_FORMAT_R16G16B16A16_FLOAT)->bytes != 8 || CopyBlockOf(DXGI_FORMAT_R11G11B10_FLOAT)->bytes != 4 || CopyBlockOf(DXGI_FORMAT_D16_UNORM)->bytes != 2 ||
        CopyBlockOf(static_cast<DXGI_FORMAT>(200))) return Fail("texel formats");
    const D3D12_RESOURCE_DESC array = TextureDesc2D(DXGI_FORMAT_R8G8B8A8_UNORM, 100, 50, 4, 3, D3D12_RESOURCE_FLAG_NONE);
    const std::optional<Subresource> sub = SplitSubresource(array, 7, 3);
    if (!sub || sub->mip != 1 || sub->layer != 2 || sub->plane != 0 || SplitSubresource(array, 12, 3)->plane != 1 || SplitSubresource(array, 24, 3)) return Fail("subresource split");
    if (MipExtent(100, 2) != 25 || MipExtent(100, 7) != 1 || MipExtent(1, 0) != 1) return Fail("mip extent");
    const std::optional<Footprints> rgba = DesktopCopyableFootprints(array, 3, 0, 3, 0, error);
    if (!rgba) return Fail("footprints: " + error);
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& mip0 = rgba->layouts[0];
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& mip1 = rgba->layouts[1];
    if (mip0.Offset != 0 || mip0.Footprint.RowPitch != 512 || rgba->rows[0] != 50 || rgba->rowBytes[0] != 400 || mip1.Offset != 25600 || mip1.Footprint.RowPitch != 256 ||
        mip1.Footprint.Width != 50 || mip1.Footprint.Height != 25) return Fail("rgba footprints");
    if (rgba->totalBytes != rgba->layouts[2].Offset + 256 * 11 + 100) return Fail("total bytes " + std::to_string(rgba->totalBytes));
    D3D12_RESOURCE_DESC blocks = TextureDesc2D(DXGI_FORMAT_BC1_UNORM, 8, 8, 1, 3, D3D12_RESOURCE_FLAG_NONE);
    const std::optional<Footprints> compressed = DesktopCopyableFootprints(blocks, 3, 0, 3, 0, error);
    if (!compressed || compressed->layouts[0].Footprint.RowPitch != 256 || compressed->rows[0] != 2 || compressed->layouts[2].Footprint.Width != 4 || compressed->rows[2] != 1) return Fail("bc footprints");
    std::optional<VkBufferImageCopy> region = FootprintRegion(array, 3, VK_IMAGE_ASPECT_COLOR_BIT, 7, mip1, 0, 0, 0, nullptr, error);
    if (!region || region->bufferRowLength != 64 || region->bufferImageHeight != 25 || region->imageSubresource.mipLevel != 1 || region->imageSubresource.baseArrayLayer != 2 ||
        region->imageExtent.width != 50 || region->imageExtent.height != 25 || region->bufferOffset != 25600) return Fail("region");
    const D3D12_BOX box{4, 2, 0, 20, 10, 1};
    region = FootprintRegion(array, 3, VK_IMAGE_ASPECT_COLOR_BIT, 0, mip0, 10, 5, 0, &box, error);
    if (!region || region->bufferOffset != 2 * 512 + 4 * 4 || region->imageOffset.x != 10 || region->imageOffset.y != 5 || region->imageExtent.width != 16 || region->imageExtent.height != 8) return Fail("boxed region");
    region = FootprintRegion(blocks, 3, VK_IMAGE_ASPECT_COLOR_BIT, 2, compressed->layouts[2], 0, 0, 0, nullptr, error);
    if (!region || region->imageExtent.width != 2 || region->imageExtent.height != 2 || region->bufferRowLength != 128) return Fail("bc edge block: " + error);
    struct Refusal {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        const D3D12_RESOURCE_DESC* texture;
        VkImageAspectFlags aspects;
        UINT subresource;
        UINT x;
        const char* expected;
    };
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT oddPitch = mip0;
    oddPitch.Footprint.RowPitch = 514;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT shortPitch = mip0;
    shortPitch.Footprint.RowPitch = 256;
    const Refusal refusals[] = {
        {oddPitch, &array, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, "not a whole number of blocks"},
        {shortPitch, &array, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, "shorter than a row"},
        {mip0, &array, VK_IMAGE_ASPECT_COLOR_BIT, 0, 60, "runs past the edge of the mip"},
        {mip0, &array, VK_IMAGE_ASPECT_COLOR_BIT, 0, 100, "starts outside mip 0"},
        {mip0, &array, VK_IMAGE_ASPECT_COLOR_BIT, 12, 0, "names a plane the texture does not have"},
        {mip0, &array, VK_IMAGE_ASPECT_COLOR_BIT, 24, 0, "outside the texture"},
        {mip0, &array, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 12, 0, "a stencil plane copy needs a footprint format of 1 byte"},
        {mip1, &array, VK_IMAGE_ASPECT_DEPTH_BIT, 12, 0, "names a plane the texture does not have"},
        {compressed->layouts[0], &blocks, VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, "block boundary"},
    };
    for (const Refusal& refusal : refusals) {
        error.clear();
        if (FootprintRegion(*refusal.texture, 3, refusal.aspects, refusal.subresource, refusal.footprint, refusal.x, 0, 0, nullptr, error) ||
            error.find(refusal.expected) == std::string::npos) return Fail(std::string("refusal '") + refusal.expected + "' gave '" + error + "'");
    }
    if (PlaneAspect(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 1) != VK_IMAGE_ASPECT_STENCIL_BIT || PlaneAspect(VK_IMAGE_ASPECT_DEPTH_BIT, 0) != VK_IMAGE_ASPECT_DEPTH_BIT ||
        PlaneAspect(VK_IMAGE_ASPECT_COLOR_BIT, 0) != VK_IMAGE_ASPECT_COLOR_BIT || PlaneAspect(VK_IMAGE_ASPECT_COLOR_BIT, 1) || PlaneAspect(VK_IMAGE_ASPECT_DEPTH_BIT, 1)) return Fail("plane aspects");
    const D3D12_BOX outside{0, 0, 0, 200, 10, 1};
    if (FootprintRegion(array, 3, VK_IMAGE_ASPECT_COLOR_BIT, 0, mip0, 0, 0, 0, &outside, error) || error.find("outside the footprint") == std::string::npos) return Fail("box outside");
    std::puts("PASS commands selftest");
    return 0;
}

struct GpuTexture {
    TextureRef ref;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::vector<VkImageView> views;
};

int CommandsGpu(bool validate) {
    DeviceRequirements requirements;
    requirements.validation = validate;
    std::string error;
    const std::unique_ptr<VulkanDevice> device = VulkanDevice::Create(requirements, error);
    if (!device) {
        std::printf("SKIP no usable Vulkan device: %s\n", error.c_str());
        return validate && error == "the Khronos validation layer is not installed" ? kNoValidation : kNoDevice;
    }
    const DeviceFunctions& vk = device->Vk();
    std::vector<GpuTexture> textures;
    const auto makeTexture = [&](const D3D12_RESOURCE_DESC& desc) -> GpuTexture* {
        const std::optional<TextureImage> image = TranslateTextureDesc(desc, error);
        if (!image) return nullptr;
        GpuTexture texture;
        texture.ref.desc = desc;
        texture.ref.layout = *image;
        if (vk.createImage(device->Handle(), &image->info, nullptr, &texture.ref.image) != VK_SUCCESS) return nullptr;
        VkMemoryRequirements needs{};
        vk.getImageMemoryRequirements(device->Handle(), texture.ref.image, &needs);
        const std::optional<std::uint32_t> type = device->MemoryType(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = type ? *type : 0;
        if (!type || vk.allocateMemory(device->Handle(), &allocate, nullptr, &texture.memory) != VK_SUCCESS ||
            vk.bindImageMemory(device->Handle(), texture.ref.image, texture.memory, 0) != VK_SUCCESS) return nullptr;
        textures.push_back(texture);
        return &textures.back();
    };
    const auto makeAttachment = [&](GpuTexture& texture, bool depth, UINT mip) -> std::optional<AttachmentRef> {
        std::optional<ImageView> view;
        if (depth) {
            D3D12_DEPTH_STENCIL_VIEW_DESC desc{texture.ref.desc.Format, D3D12_DSV_DIMENSION_TEXTURE2D, D3D12_DSV_FLAG_NONE, {}};
            desc.Texture2D.MipSlice = mip;
            view = DepthStencilView(texture.ref.desc, &desc, error);
        } else {
            D3D12_RENDER_TARGET_VIEW_DESC desc{texture.ref.desc.Format, texture.ref.desc.SampleDesc.Count > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D, {}};
            desc.Texture2D.MipSlice = mip;
            view = RenderTargetView(texture.ref.desc, &desc, error);
        }
        if (!view) return std::nullopt;
        view->info.image = texture.ref.image;
        VkImageView handle = VK_NULL_HANDLE;
        if (vk.createImageView(device->Handle(), &view->info, nullptr, &handle) != VK_SUCCESS) return std::nullopt;
        texture.views.push_back(handle);
        return AttachmentRef{handle, {MipExtent(texture.ref.desc.Width, mip), MipExtent(texture.ref.desc.Height, mip)}, 1, view->info.subresourceRange.aspectMask};
    };
    textures.reserve(8);
    const D3D12_RESOURCE_FLAGS rt = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    GpuTexture* color = makeTexture(TextureDesc2D(DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 3, rt));
    GpuTexture* target = makeTexture(TextureDesc2D(DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 1, rt));
    GpuTexture* depth = makeTexture(TextureDesc2D(DXGI_FORMAT_D32_FLOAT, 32, 32, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL));
    D3D12_RESOURCE_DESC msaaDesc = TextureDesc2D(DXGI_FORMAT_R8G8B8A8_UNORM, 32, 32, 1, 1, rt);
    msaaDesc.SampleDesc = {4, 0};
    GpuTexture* msaa = makeTexture(msaaDesc);
    GpuTexture* resolved = makeTexture(TextureDesc2D(DXGI_FORMAT_R8G8B8A8_UNORM, 32, 32, 1, 1, D3D12_RESOURCE_FLAG_NONE));
    const bool bcSupported = device->HasFeature("textureCompressionBC");
    GpuTexture* blocks = bcSupported ? makeTexture(TextureDesc2D(DXGI_FORMAT_BC1_UNORM, 64, 64, 1, 1, D3D12_RESOURCE_FLAG_NONE)) : nullptr;
    if (!color || !target || !depth || !msaa || !resolved || (bcSupported && !blocks)) return Fail("textures: " + error);
    const std::optional<AttachmentRef> colorView = makeAttachment(*color, false, 0);
    const std::optional<AttachmentRef> targetView = makeAttachment(*target, false, 0);
    const std::optional<AttachmentRef> depthView = makeAttachment(*depth, true, 0);
    const std::optional<AttachmentRef> msaaView = makeAttachment(*msaa, false, 0);
    if (!colorView || !targetView || !depthView || !msaaView) return Fail("views: " + error);

    const VkDeviceSize bufferBytes = 1u << 20;
    DeviceBuffer upload;
    DeviceBuffer readback;
    if (!CreateHostBuffer(*device, bufferBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, upload, error) ||
        !CreateHostBuffer(*device, bufferBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback, error)) return Fail("buffers: " + error);
    std::uint8_t* const up = static_cast<std::uint8_t*>(upload.mapped);
    std::uint8_t* const down = static_cast<std::uint8_t*>(readback.mapped);
    std::fill(down, down + bufferBytes, std::uint8_t{0xCD});
    const BufferRef uploadRef{upload.buffer, 0, bufferBytes};
    const BufferRef readbackRef{readback.buffer, 0, bufferBytes};

    const std::optional<Footprints> colorPrints = DesktopCopyableFootprints(color->ref.desc, 3, 0, 3, 0, error);
    const std::optional<Footprints> targetPrints = DesktopCopyableFootprints(target->ref.desc, 1, 0, 1, 0x20000, error);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT mip1Back = colorPrints->layouts[1];
    mip1Back.Offset += 0x40000;
    const std::optional<Footprints> depthPrints = DesktopCopyableFootprints(depth->ref.desc, 1, 0, 1, 0x60000, error);
    const std::optional<Footprints> resolvedPrints = DesktopCopyableFootprints(resolved->ref.desc, 1, 0, 1, 0x70000, error);
    const std::optional<Footprints> blockPrints = bcSupported ? DesktopCopyableFootprints(blocks->ref.desc, 1, 0, 1, 0x80000, error) : std::nullopt;
    if (!colorPrints || !targetPrints || !depthPrints || !resolvedPrints || (bcSupported && !blockPrints)) return Fail("footprints: " + error);
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& mip1 = colorPrints->layouts[1];
    for (UINT row = 0; row < 32; ++row) {
        for (UINT column = 0; column < 32 * 4; ++column) up[mip1.Offset + row * mip1.Footprint.RowPitch + column] = static_cast<std::uint8_t>(row * 7 + column);
    }
    if (bcSupported) {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& print = blockPrints->layouts[0];
        for (UINT row = 0; row < 16; ++row) {
            for (UINT column = 0; column < 16 * 8; ++column) up[print.Offset + row * print.Footprint.RowPitch + column] = static_cast<std::uint8_t>(row * 13 + column * 3 + 1);
        }
    }
    for (VkDeviceSize index = 0; index < 4096; ++index) up[0x90000 + index] = static_cast<std::uint8_t>(index * 5 + 3);

    bool recorded = true;
    std::string step;
    const bool submitted = device->Submit(
        [&](VkCommandBuffer commands) {
            CommandRecorder recorder(*device, commands);
            const auto check = [&](bool ok, const char* name) {
                if (!ok && recorded) {
                    recorded = false;
                    step = name;
                }
            };
            const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
            const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
            const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            for (const GpuTexture& texture : textures) check(recorder.InitializeTexture(texture.ref, D3D12_RESOURCE_STATE_COMMON, error), "initialize");
            check(recorder.Transition(color->ref, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET, error), "color to render target");
            check(recorder.ClearRenderTarget(*colorView, red, {}, error), "clear red");
            check(recorder.ClearRenderTarget(*colorView, green, {{8, 8, 24, 24}}, error), "clear green rect");
            check(recorder.Transition(target->ref, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET, error), "target to render target");
            check(recorder.ClearRenderTarget(*targetView, blue, {}, error), "clear blue");
            check(recorder.Transition(color->ref, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST, error), "color to copy dest");
            check(recorder.CopyBufferToTexture(color->ref, D3D12_RESOURCE_STATE_COPY_DEST, 1, 0, 0, 0, uploadRef, mip1, nullptr, error), "upload mip 1");
            check(recorder.Transition(color->ref, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE, error), "color to copy source");
            check(recorder.Transition(target->ref, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST, error), "target to copy dest");
            const D3D12_BOX square{8, 8, 0, 24, 24, 1};
            check(recorder.CopyTexture(target->ref, D3D12_RESOURCE_STATE_COPY_DEST, 0, 4, 4, 0, color->ref, D3D12_RESOURCE_STATE_COPY_SOURCE, 0, &square, error), "texture copy");
            check(recorder.Transition(target->ref, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE, error), "target to copy source");
            check(recorder.CopyTextureToBuffer(readbackRef, colorPrints->layouts[0], color->ref, D3D12_RESOURCE_STATE_COPY_SOURCE, 0, nullptr, error), "read mip 0");
            check(recorder.CopyTextureToBuffer(readbackRef, mip1Back, color->ref, D3D12_RESOURCE_STATE_COPY_SOURCE, 1, nullptr, error), "read mip 1");
            check(recorder.CopyTextureToBuffer(readbackRef, targetPrints->layouts[0], target->ref, D3D12_RESOURCE_STATE_COPY_SOURCE, 0, nullptr, error), "read target");
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT offsetPrint{};
            offsetPrint.Offset = 0xB0000;
            offsetPrint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 32, 32, 1, 256};
            const D3D12_BOX greenSquare{8, 8, 0, 24, 24, 1};
            check(recorder.CopyTextureToFootprint(readbackRef, offsetPrint, 4, 2, 0, color->ref, D3D12_RESOURCE_STATE_COPY_SOURCE, 0, &greenSquare, error), "read into a footprint at (4,2)");
            check(!recorder.CopyTextureToFootprint(readbackRef, offsetPrint, 20, 0, 0, color->ref, D3D12_RESOURCE_STATE_COPY_SOURCE, 0, &greenSquare, error), "past the footprint refused");
            check(!recorder.CopyTextureToFootprint(readbackRef, offsetPrint, 0, 0, 0, color->ref, D3D12_RESOURCE_STATE_COPY_SOURCE, 0, nullptr, error), "a whole 64x64 mip into a 32x32 footprint refused");
            check(recorder.Transition(depth->ref, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE, error), "depth to write");
            check(recorder.ClearDepthStencil(*depthView, D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0, {}, error), "clear depth");
            check(recorder.ClearDepthStencil(*depthView, D3D12_CLEAR_FLAG_DEPTH, 0.75f, 0, {{0, 0, 8, 8}}, error), "clear depth rect");
            check(recorder.Transition(depth->ref, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE, error), "depth to copy source");
            check(recorder.CopyTextureToBuffer(readbackRef, depthPrints->layouts[0], depth->ref, D3D12_RESOURCE_STATE_COPY_SOURCE, 0, nullptr, error), "read depth");
            check(recorder.Transition(msaa->ref, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET, error), "msaa to render target");
            check(recorder.ClearRenderTarget(*msaaView, blue, {}, error), "clear msaa");
            check(recorder.Transition(msaa->ref, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, error), "msaa to resolve source");
            check(recorder.Transition(resolved->ref, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RESOLVE_DEST, error), "resolved to resolve dest");
            check(recorder.ResolveSubresource(resolved->ref, D3D12_RESOURCE_STATE_RESOLVE_DEST, 0, msaa->ref, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, 0, error), "resolve");
            check(recorder.Transition(resolved->ref, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE, error), "resolved to copy source");
            check(recorder.CopyTextureToBuffer(readbackRef, resolvedPrints->layouts[0], resolved->ref, D3D12_RESOURCE_STATE_COPY_SOURCE, 0, nullptr, error), "read resolved");
            if (bcSupported) {
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT back = blockPrints->layouts[0];
                back.Offset += 0x8000;
                check(recorder.Transition(blocks->ref, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST, error), "blocks to copy dest");
                check(recorder.CopyBufferToTexture(blocks->ref, D3D12_RESOURCE_STATE_COPY_DEST, 0, 0, 0, 0, uploadRef, blockPrints->layouts[0], nullptr, error), "upload blocks");
                check(recorder.Transition(blocks->ref, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE, error), "blocks to copy source");
                check(recorder.CopyTextureToBuffer(readbackRef, back, blocks->ref, D3D12_RESOURCE_STATE_COPY_SOURCE, 0, nullptr, error), "read blocks");
            }
            check(recorder.CopyBufferRegion(readbackRef, 0xA0000, uploadRef, 0x90000, 4096, error), "buffer copy");
            check(!recorder.CopyBufferRegion(readbackRef, bufferBytes - 16, uploadRef, 0, 32, error), "buffer copy bounds");
            VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            host.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &host;
            vk.cmdPipelineBarrier2(commands, &dependency);
        },
        error);
    int status = 0;
    if (!submitted || !recorded) status = Fail("recording '" + step + "': " + error);
    const auto pixel = [&](const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& print, UINT x, UINT y) {
        const std::uint8_t* p = down + print.Offset + y * print.Footprint.RowPitch + x * 4;
        return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 | static_cast<std::uint32_t>(p[2]) << 8 | p[3];
    };
    std::size_t checks = 0;
    const auto expect = [&](bool ok, const std::string& what) {
        ++checks;
        if (!ok && status == 0) status = Fail(what);
    };
    if (status == 0) {
        expect(pixel(colorPrints->layouts[0], 0, 0) == 0xFF0000FFu, "cleared red at (0,0)");
        expect(pixel(colorPrints->layouts[0], 10, 10) == 0x00FF00FFu, "green rectangle at (10,10)");
        expect(pixel(colorPrints->layouts[0], 23, 23) == 0x00FF00FFu, "green rectangle at (23,23)");
        expect(pixel(colorPrints->layouts[0], 24, 24) == 0xFF0000FFu, "red past the rectangle at (24,24)");
        expect(pixel(colorPrints->layouts[0], 63, 63) == 0xFF0000FFu, "red at (63,63)");
        bool same = true;
        for (UINT row = 0; row < 32 && same; ++row) {
            same = std::memcmp(down + mip1Back.Offset + row * mip1Back.Footprint.RowPitch, up + mip1.Offset + row * mip1.Footprint.RowPitch, 32 * 4) == 0;
        }
        expect(same, "mip 1 read back as uploaded");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT offsetPrint{};
        offsetPrint.Offset = 0xB0000;
        offsetPrint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 32, 32, 1, 256};
        expect(pixel(offsetPrint, 4, 2) == 0x00FF00FFu && pixel(offsetPrint, 19, 17) == 0x00FF00FFu, "the green square lands at (4,2) in the footprint");
        expect(pixel(offsetPrint, 3, 2) == 0xCDCDCDCDu && pixel(offsetPrint, 20, 17) == 0xCDCDCDCDu && pixel(offsetPrint, 4, 18) == 0xCDCDCDCDu, "the footprint is untouched around the square");
        expect(pixel(targetPrints->layouts[0], 0, 0) == 0x0000FFFFu, "blue target at (0,0)");
        expect(pixel(targetPrints->layouts[0], 4, 4) == 0x00FF00FFu, "copied square at (4,4)");
        expect(pixel(targetPrints->layouts[0], 19, 19) == 0x00FF00FFu, "copied square at (19,19)");
        expect(pixel(targetPrints->layouts[0], 20, 20) == 0x0000FFFFu, "blue past the copied square at (20,20)");
        float nearDepth = 0.0f;
        float farDepth = 0.0f;
        std::memcpy(&nearDepth, down + depthPrints->layouts[0].Offset, sizeof(nearDepth));
        std::memcpy(&farDepth, down + depthPrints->layouts[0].Offset + 16 * depthPrints->layouts[0].Footprint.RowPitch + 16 * 4, sizeof(farDepth));
        expect(nearDepth == 0.75f && farDepth == 0.25f, "depth read back as " + std::to_string(nearDepth) + " and " + std::to_string(farDepth));
        expect(pixel(resolvedPrints->layouts[0], 5, 5) == 0x0000FFFFu, "resolved blue");
        if (bcSupported) {
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& print = blockPrints->layouts[0];
            bool blocksSame = true;
            for (UINT row = 0; row < 16 && blocksSame; ++row) {
                blocksSame = std::memcmp(down + print.Offset + 0x8000 + row * print.Footprint.RowPitch, up + print.Offset + row * print.Footprint.RowPitch, 16 * 8) == 0;
            }
            expect(blocksSame, "BC1 blocks read back as uploaded");
        }
        expect(std::memcmp(down + 0xA0000, up + 0x90000, 4096) == 0, "buffer copy");
    }
    std::printf("commands: %zu read-back checks%s\n", checks, bcSupported ? "" : " (no BC support: block copy skipped)");
    for (GpuTexture& texture : textures) {
        for (VkImageView view : texture.views) vk.destroyImageView(device->Handle(), view, nullptr);
        vk.destroyImage(device->Handle(), texture.ref.image, nullptr);
        vk.freeMemory(device->Handle(), texture.memory, nullptr);
    }
    DestroyBuffer(*device, upload);
    DestroyBuffer(*device, readback);
    if (validate) {
        std::printf("validation errors=%zu\n", device->ValidationErrors());
        for (const std::string& message : device->ValidationMessages()) std::printf("  %.600s\n", message.c_str());
        if (device->ValidationErrors() != 0) status = 1;
    }
    if (status == 0) std::puts("PASS commands gpu");
    return status;
}

const std::uint32_t kCornerVertex[] = {
#include "shaders/graphics_corner.inc"
};

const std::uint32_t kDispatchFill[] = {
#include "shaders/dispatch_fill.inc"
};

int DrawSelfTest() {
    if (VulkanTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || VulkanTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ) != VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY ||
        VulkanTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST) != VK_PRIMITIVE_TOPOLOGY_POINT_LIST || VulkanTopology(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST) ||
        VulkanTopology(D3D_PRIMITIVE_TOPOLOGY_UNDEFINED)) return Fail("topologies");
    const std::optional<VkViewport> viewport = VulkanViewport({10.0f, 20.0f, 300.0f, 200.0f, 0.1f, 0.9f});
    if (!viewport || viewport->x != 10.0f || viewport->y != 220.0f || viewport->width != 300.0f || viewport->height != -200.0f || viewport->minDepth != 0.1f || viewport->maxDepth != 0.9f) return Fail("viewport flip");
    if (!VulkanViewport({0.0f, 0.0f, 8.0f, 8.0f, 1.0f, 0.0f})) return Fail("inverted depth range");
    if (VulkanViewport({0.0f, 0.0f, 0.0f, 8.0f, 0.0f, 1.0f}) || VulkanViewport({0.0f, 0.0f, 8.0f, 8.0f, 0.0f, 1.5f}) ||
        VulkanViewport({0.0f, std::numeric_limits<float>::quiet_NaN(), 8.0f, 8.0f, 0.0f, 1.0f})) return Fail("bad viewports");
    std::puts("PASS draw selftest");
    return 0;
}

int DrawGpu(bool validate) {
    DeviceRequirements requirements;
    requirements.features = {"bufferDeviceAddress", "shaderInt64"};
    requirements.validation = validate;
    std::string error;
    const std::unique_ptr<VulkanDevice> device = VulkanDevice::Create(requirements, error);
    if (!device) {
        std::printf("SKIP no usable Vulkan device: %s\n", error.c_str());
        return validate && error == "the Khronos validation layer is not installed" ? kNoValidation : kNoDevice;
    }
    const DeviceFunctions& vk = device->Vk();
    const auto makeModule = [&](const std::uint32_t* words, std::size_t bytes) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = bytes;
        info.pCode = words;
        VkShaderModule module = VK_NULL_HANDLE;
        return vk.createShaderModule(device->Handle(), &info, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
    };
    const VkShaderModule corner = makeModule(kCornerVertex, sizeof(kCornerVertex));
    const VkShaderModule inputs = makeModule(kGraphicsInputs, sizeof(kGraphicsInputs));
    const VkShaderModule fragment = makeModule(kGraphicsFloat, sizeof(kGraphicsFloat));
    const VkShaderModule fill = makeModule(kDispatchFill, sizeof(kDispatchFill));
    if (!corner || !inputs || !fragment || !fill) return Fail("shader modules");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = DefaultGraphics();
    desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    const std::optional<GraphicsPipelineState> front = TranslateGraphicsState(desc, error);
    desc.RasterizerState.FrontCounterClockwise = TRUE;
    const std::optional<GraphicsPipelineState> flipped = TranslateGraphicsState(desc, error);
    const std::optional<VertexInput> layout = TranslateInputLayout({kTestLayout, 4}, TestShaderInputs(), error);
    if (!front || !flipped || !layout) return Fail("states: " + error);
    VkPipeline drawn = VK_NULL_HANDLE;
    VkPipeline culled = VK_NULL_HANDLE;
    VkPipeline buffered = VK_NULL_HANDLE;
    if (CreateGraphicsPipeline(*device, VK_NULL_HANDLE, *front, {{VK_SHADER_STAGE_VERTEX_BIT, corner, 0}, {VK_SHADER_STAGE_FRAGMENT_BIT, fragment, 0}}, {}, 0, &drawn, error) != VK_SUCCESS ||
        CreateGraphicsPipeline(*device, VK_NULL_HANDLE, *flipped, {{VK_SHADER_STAGE_VERTEX_BIT, corner, 0}, {VK_SHADER_STAGE_FRAGMENT_BIT, fragment, 0}}, {}, 0, &culled, error) != VK_SUCCESS ||
        CreateGraphicsPipeline(*device, VK_NULL_HANDLE, *front, {{VK_SHADER_STAGE_VERTEX_BIT, inputs, 0}, {VK_SHADER_STAGE_FRAGMENT_BIT, fragment, 0}}, *layout, 0, &buffered, error) != VK_SUCCESS) {
        return Fail("graphics pipelines: " + error);
    }
    VkComputePipelineCreateInfo computeInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    computeInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, fill, "main", nullptr};
    computeInfo.layout = device->TitlePipelineLayout();
    VkPipeline compute = VK_NULL_HANDLE;
    if (vk.createComputePipelines(device->Handle(), VK_NULL_HANDLE, 1, &computeInfo, nullptr, &compute) != VK_SUCCESS) return Fail("compute pipeline");

    const D3D12_RESOURCE_DESC targetDesc = TextureDesc2D(DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    TextureRef target;
    target.desc = targetDesc;
    target.layout = *TranslateTextureDesc(targetDesc, error);
    VkDeviceMemory targetMemory = VK_NULL_HANDLE;
    if (vk.createImage(device->Handle(), &target.layout.info, nullptr, &target.image) != VK_SUCCESS) return Fail("target image");
    VkMemoryRequirements needs{};
    vk.getImageMemoryRequirements(device->Handle(), target.image, &needs);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = needs.size;
    allocate.memoryTypeIndex = device->MemoryType(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT).value_or(0);
    if (vk.allocateMemory(device->Handle(), &allocate, nullptr, &targetMemory) != VK_SUCCESS || vk.bindImageMemory(device->Handle(), target.image, targetMemory, 0) != VK_SUCCESS) return Fail("target memory");
    std::optional<ImageView> rtv = RenderTargetView(targetDesc, nullptr, error);
    rtv->info.image = target.image;
    VkImageView targetView = VK_NULL_HANDLE;
    if (vk.createImageView(device->Handle(), &rtv->info, nullptr, &targetView) != VK_SUCCESS) return Fail("target view");
    const AttachmentRef attachment{targetView, {64, 64}, 1, VK_IMAGE_ASPECT_COLOR_BIT};

    DeviceBuffer host;
    const VkDeviceSize hostBytes = 1u << 20;
    if (!CreateHostBuffer(*device, hostBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, host, error)) return Fail("host buffer: " + error);
    std::uint8_t* const bytes = static_cast<std::uint8_t*>(host.mapped);
    std::fill(bytes, bytes + hostBytes, std::uint8_t{0});
    const BufferRef hostRef{host.buffer, 0, hostBytes};
    struct Vertex {
        float position[3];
        std::uint8_t color[4];
        std::uint16_t texcoord[2];
    };
    static_assert(sizeof(Vertex) == 20);
    const Vertex vertices[3] = {{{1.0f, -1.0f, 0.0f}, {0, 0, 0, 0}, {0, 0}}, {{-1.0f, -1.0f, 0.0f}, {0, 0, 0, 0}, {0, 0}}, {{1.0f, 1.0f, 0.0f}, {0, 0, 0, 0}, {0, 0}}};
    const VkDeviceSize vertexOffset = 0x80000;
    const VkDeviceSize instanceOffset = 0x80100;
    const VkDeviceSize indexOffset = 0x80200;
    const VkDeviceSize computeOffset = 0x90000;
    std::memcpy(bytes + vertexOffset, vertices, sizeof(vertices));
    const std::uint16_t indices[3] = {0, 1, 2};
    std::memcpy(bytes + indexOffset, indices, sizeof(indices));
    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = host.buffer;
    const VkDeviceAddress address = vk.getBufferDeviceAddress(device->Handle(), &addressInfo);

    const std::optional<Footprints> prints = DesktopCopyableFootprints(targetDesc, 1, 0, 1, 0, error);
    const UINT pitch = prints->layouts[0].Footprint.RowPitch;
    const UINT64 regionBytes = 0x10000;
    bool recorded = true;
    std::string step;
    const bool submitted = device->Submit(
        [&](VkCommandBuffer commands) {
            CommandRecorder recorder(*device, commands);
            const auto check = [&](bool ok, const char* name) {
                if (!ok && recorded) {
                    recorded = false;
                    step = name;
                }
            };
            const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            check(recorder.InitializeTexture(target, D3D12_RESOURCE_STATE_RENDER_TARGET, error), "initialize");
            const auto pass = [&](VkPipeline pipeline, const D3D12_VIEWPORT& viewport, bool indexed, UINT region) {
                check(recorder.ClearRenderTarget(attachment, blue, {}, error), "clear");
                check(recorder.BeginRendering({attachment}, nullptr, {64, 64}, 1, error), "begin rendering");
                check(recorder.SetViewports({viewport}, error), "viewport");
                check(recorder.SetScissors({{0, 0, 64, 64}}, error), "scissor");
                check(recorder.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, false, error), "topology");
                recorder.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                if (indexed) {
                    check(recorder.SetVertexBuffers(0, {{{host.buffer, vertexOffset, sizeof(vertices)}, sizeof(Vertex)}}, error), "vertex buffer");
                    check(recorder.SetVertexBuffers(2, {{{host.buffer, instanceOffset, 64}, 8}}, error), "instance buffer");
                    check(recorder.SetIndexBuffer({host.buffer, indexOffset, sizeof(indices)}, DXGI_FORMAT_R16_UINT, error), "index buffer");
                    recorder.DrawIndexed(3, 1, 0, 0, 0);
                } else {
                    recorder.Draw(3, 1, 0, 0);
                }
                recorder.EndRendering();
                check(recorder.Transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE, error), "to copy source");
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = prints->layouts[0];
                footprint.Offset = region * regionBytes;
                check(recorder.CopyTextureToBuffer(hostRef, footprint, target, D3D12_RESOURCE_STATE_COPY_SOURCE, 0, nullptr, error), "read back");
                check(recorder.Transition(target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET, error), "to render target");
            };
            pass(drawn, {0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f}, false, 0);
            pass(culled, {0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f}, false, 1);
            pass(drawn, {32.0f, 0.0f, 32.0f, 64.0f, 0.0f, 1.0f}, false, 2);
            pass(buffered, {0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f}, true, 3);
            struct {
                std::uint64_t address;
                std::uint32_t base;
                std::uint32_t count;
            } constants{address + computeOffset, 1000, 256};
            recorder.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, compute);
            check(recorder.SetPushConstants(&constants, sizeof(constants), error), "push constants");
            check(!recorder.SetPushConstants(&constants, 148, error), "push constants over 144 bytes");
            recorder.Dispatch(4, 1, 1);
            VkMemoryBarrier2 hostBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            hostBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            hostBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            hostBarrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            hostBarrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &hostBarrier;
            vk.cmdPipelineBarrier2(commands, &dependency);
        },
        error);
    int status = 0;
    if (!submitted || !recorded) status = Fail("recording '" + step + "': " + error);
    const auto rgb = [&](UINT region, UINT x, UINT y) {
        const std::uint8_t* p = bytes + region * regionBytes + y * pitch + x * 4;
        return static_cast<std::uint32_t>(p[0]) << 16 | static_cast<std::uint32_t>(p[1]) << 8 | p[2];
    };
    std::size_t checks = 0;
    const auto expect = [&](bool ok, const std::string& what) {
        ++checks;
        if (!ok && status == 0) status = Fail(what);
    };
    if (status == 0) {
        const std::uint32_t red = 0xFF0000u;
        const std::uint32_t blueRgb = 0x0000FFu;
        expect(rgb(0, 4, 4) == red && rgb(0, 40, 10) == red && rgb(0, 10, 40) == red, "the clockwise upper-left triangle is drawn");
        expect(rgb(0, 60, 60) == blueRgb && rgb(0, 40, 40) == blueRgb, "the lower-right half stays clear");
        expect(rgb(1, 4, 4) == blueRgb && rgb(1, 20, 20) == blueRgb, "the same triangle is culled when counter-clockwise faces are front");
        expect(rgb(2, 36, 4) == red && rgb(2, 4, 4) == blueRgb && rgb(2, 62, 40) == blueRgb, "the viewport moves the triangle into the right half");
        expect(rgb(3, 60, 60) == red && rgb(3, 40, 40) == red && rgb(3, 4, 4) == blueRgb && rgb(3, 10, 10) == blueRgb, "the indexed triangle from the vertex buffer covers the lower-right half");
        const std::uint8_t alpha = bytes[4 * pitch + 4 * 4 + 3];
        expect(alpha == 0x7F || alpha == 0x80, "pixel shader alpha 0.5 stored as " + std::to_string(alpha));
        bool filled = true;
        for (std::uint32_t item = 0; item < 256 && filled; ++item) {
            std::uint32_t value = 0;
            std::memcpy(&value, bytes + computeOffset + item * 4, sizeof(value));
            filled = value == 1000 + item * 3;
        }
        expect(filled, "the dispatch filled 256 values through the pushed buffer address");
    }
    std::printf("draw: %zu read-back checks\n", checks);
    vk.destroyImageView(device->Handle(), targetView, nullptr);
    vk.destroyImage(device->Handle(), target.image, nullptr);
    vk.freeMemory(device->Handle(), targetMemory, nullptr);
    DestroyBuffer(*device, host);
    for (VkPipeline pipeline : {drawn, culled, buffered, compute}) vk.destroyPipeline(device->Handle(), pipeline, nullptr);
    for (VkShaderModule module : {corner, inputs, fragment, fill}) vk.destroyShaderModule(device->Handle(), module, nullptr);
    if (validate) {
        std::printf("validation errors=%zu\n", device->ValidationErrors());
        for (const std::string& message : device->ValidationMessages()) std::printf("  %.600s\n", message.c_str());
        if (device->ValidationErrors() != 0) status = 1;
    }
    if (status == 0) std::puts("PASS draw gpu");
    return status;
}

const std::uint32_t kQuadrantVertex[] = {
#include "shaders/graphics_quadrant.inc"
};

int IndirectSelfTest() {
    std::string error;
    if (IndirectArgumentBytes(IndirectKind::Draw) != 16 || IndirectArgumentBytes(IndirectKind::DrawIndexed) != 20 || IndirectArgumentBytes(IndirectKind::Dispatch) != 12) return Fail("argument sizes");
    static_assert(sizeof(D3D12_DRAW_ARGUMENTS) == sizeof(VkDrawIndirectCommand));
    static_assert(sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) == sizeof(VkDrawIndexedIndirectCommand));
    static_assert(sizeof(D3D12_DISPATCH_ARGUMENTS) == sizeof(VkDispatchIndirectCommand));
    static_assert(offsetof(D3D12_DRAW_INDEXED_ARGUMENTS, BaseVertexLocation) == offsetof(VkDrawIndexedIndirectCommand, vertexOffset));
    D3D12_INDIRECT_ARGUMENT_DESC draw{};
    draw.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_INDIRECT_ARGUMENT_DESC indexed{};
    indexed.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_INDIRECT_ARGUMENT_DESC dispatch{};
    dispatch.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_INDIRECT_ARGUMENT_DESC constant{};
    constant.Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    D3D12_INDIRECT_ARGUMENT_DESC mesh{};
    mesh.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
    std::optional<IndirectSignature> signature = TranslateCommandSignature({32, 1, &draw, 0}, error);
    if (!signature || signature->kind != IndirectKind::Draw || signature->stride != 32) return Fail("draw signature: " + error);
    signature = TranslateCommandSignature({20, 1, &indexed, 1}, error);
    if (!signature || signature->kind != IndirectKind::DrawIndexed) return Fail("indexed signature: " + error);
    signature = TranslateCommandSignature({12, 1, &dispatch, 0}, error);
    if (!signature || signature->kind != IndirectKind::Dispatch) return Fail("dispatch signature: " + error);
    const D3D12_INDIRECT_ARGUMENT_DESC withConstant[] = {constant, draw};
    const D3D12_INDIRECT_ARGUMENT_DESC drawFirst[] = {draw, constant};
    const std::pair<D3D12_COMMAND_SIGNATURE_DESC, const char*> refusals[] = {
        {{16, 2, withConstant, 0}, "argument type 5 (constants, views or buffer bindings changed per command)"},
        {{16, 2, drawFirst, 0}, "is not the last argument"},
        {{16, 1, &mesh, 0}, "type 10 as the last argument"},
        {{12, 1, &draw, 0}, "byte stride 12 is smaller than the 16-byte arguments"},
        {{18, 1, &draw, 0}, "byte stride 18"},
        {{16, 0, &draw, 0}, "has no arguments"},
        {{16, 1, &draw, 2}, "node mask 2"},
    };
    for (const auto& [desc, expected] : refusals) {
        error.clear();
        if (TranslateCommandSignature(desc, error) || error.find(expected) == std::string::npos) return Fail(std::string("refusal '") + expected + "' gave '" + error + "'");
    }
    std::puts("PASS indirect selftest");
    return 0;
}

int IndirectGpu(bool validate) {
    DeviceRequirements requirements;
    requirements.features = {"bufferDeviceAddress", "shaderInt64"};
    requirements.validation = validate;
    std::string error;
    const std::unique_ptr<VulkanDevice> device = VulkanDevice::Create(requirements, error);
    if (!device) {
        std::printf("SKIP no usable Vulkan device: %s\n", error.c_str());
        return validate && error == "the Khronos validation layer is not installed" ? kNoValidation : kNoDevice;
    }
    const DeviceFunctions& vk = device->Vk();
    const bool counted = device->HasFeature("drawIndirectCount");
    std::printf("multiDrawIndirect %s, drawIndirectCount %s\n", device->HasFeature("multiDrawIndirect") ? "yes" : "no", counted ? "yes" : "no");
    const auto makeModule = [&](const std::uint32_t* words, std::size_t bytes) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = bytes;
        info.pCode = words;
        VkShaderModule module = VK_NULL_HANDLE;
        return vk.createShaderModule(device->Handle(), &info, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
    };
    const VkShaderModule quadrant = makeModule(kQuadrantVertex, sizeof(kQuadrantVertex));
    const VkShaderModule fragment = makeModule(kGraphicsFloat, sizeof(kGraphicsFloat));
    const VkShaderModule fill = makeModule(kDispatchFill, sizeof(kDispatchFill));
    if (!quadrant || !fragment || !fill) return Fail("shader modules");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = DefaultGraphics();
    desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    const std::optional<GraphicsPipelineState> state = TranslateGraphicsState(desc, error);
    VkPipeline graphics = VK_NULL_HANDLE;
    if (!state || CreateGraphicsPipeline(*device, VK_NULL_HANDLE, *state, {{VK_SHADER_STAGE_VERTEX_BIT, quadrant, 0}, {VK_SHADER_STAGE_FRAGMENT_BIT, fragment, 0}}, {}, 0, &graphics, error) != VK_SUCCESS) {
        return Fail("graphics pipeline: " + error);
    }
    VkComputePipelineCreateInfo computeInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    computeInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, fill, "main", nullptr};
    computeInfo.layout = device->TitlePipelineLayout();
    VkPipeline compute = VK_NULL_HANDLE;
    if (vk.createComputePipelines(device->Handle(), VK_NULL_HANDLE, 1, &computeInfo, nullptr, &compute) != VK_SUCCESS) return Fail("compute pipeline");

    const D3D12_RESOURCE_DESC targetDesc = TextureDesc2D(DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    TextureRef target;
    target.desc = targetDesc;
    target.layout = *TranslateTextureDesc(targetDesc, error);
    VkDeviceMemory targetMemory = VK_NULL_HANDLE;
    if (vk.createImage(device->Handle(), &target.layout.info, nullptr, &target.image) != VK_SUCCESS) return Fail("target image");
    VkMemoryRequirements needs{};
    vk.getImageMemoryRequirements(device->Handle(), target.image, &needs);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = needs.size;
    allocate.memoryTypeIndex = device->MemoryType(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT).value_or(0);
    if (vk.allocateMemory(device->Handle(), &allocate, nullptr, &targetMemory) != VK_SUCCESS || vk.bindImageMemory(device->Handle(), target.image, targetMemory, 0) != VK_SUCCESS) return Fail("target memory");
    std::optional<ImageView> rtv = RenderTargetView(targetDesc, nullptr, error);
    rtv->info.image = target.image;
    VkImageView targetView = VK_NULL_HANDLE;
    if (vk.createImageView(device->Handle(), &rtv->info, nullptr, &targetView) != VK_SUCCESS) return Fail("target view");
    const AttachmentRef attachment{targetView, {64, 64}, 1, VK_IMAGE_ASPECT_COLOR_BIT};

    DeviceBuffer host;
    const VkDeviceSize hostBytes = 1u << 20;
    if (!CreateHostBuffer(*device, hostBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, host, error)) return Fail("host buffer: " + error);
    std::uint8_t* const bytes = static_cast<std::uint8_t*>(host.mapped);
    std::fill(bytes, bytes + hostBytes, std::uint8_t{0});
    const BufferRef hostRef{host.buffer, 0, hostBytes};
    const VkDeviceSize drawArgs = 0x80000;
    const VkDeviceSize indexedArgs = 0x80100;
    const VkDeviceSize dispatchArgs = 0x80200;
    const VkDeviceSize countOne = 0x80300;
    const VkDeviceSize countZero = 0x80304;
    const VkDeviceSize indices = 0x80400;
    const VkDeviceSize fillAt = 0x90000;
    const D3D12_DRAW_ARGUMENTS draws[2] = {{3, 1, 0, 0}, {3, 1, 3, 0}};
    std::memcpy(bytes + drawArgs, &draws[0], sizeof(draws[0]));
    std::memcpy(bytes + drawArgs + 32, &draws[1], sizeof(draws[1]));
    const D3D12_DRAW_INDEXED_ARGUMENTS indexedDraw{3, 1, 0, 0, 0};
    std::memcpy(bytes + indexedArgs, &indexedDraw, sizeof(indexedDraw));
    const D3D12_DISPATCH_ARGUMENTS groups{2, 1, 1};
    std::memcpy(bytes + dispatchArgs, &groups, sizeof(groups));
    const std::uint32_t one = 1;
    std::memcpy(bytes + countOne, &one, sizeof(one));
    const std::uint16_t lowerRight[3] = {3, 4, 5};
    std::memcpy(bytes + indices, lowerRight, sizeof(lowerRight));
    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = host.buffer;
    const VkDeviceAddress address = vk.getBufferDeviceAddress(device->Handle(), &addressInfo);
    const IndirectSignature drawSignature{IndirectKind::Draw, 32};
    const IndirectSignature indexedSignature{IndirectKind::DrawIndexed, 20};
    const IndirectSignature dispatchSignature{IndirectKind::Dispatch, 12};

    const std::optional<Footprints> prints = DesktopCopyableFootprints(targetDesc, 1, 0, 1, 0, error);
    const UINT pitch = prints->layouts[0].Footprint.RowPitch;
    const UINT64 regionBytes = 0x10000;
    bool recorded = true;
    std::string step;
    const bool submitted = device->Submit(
        [&](VkCommandBuffer commands) {
            CommandRecorder recorder(*device, commands);
            const auto check = [&](bool ok, const char* name) {
                if (!ok && recorded) {
                    recorded = false;
                    step = name;
                }
            };
            const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            check(recorder.InitializeTexture(target, D3D12_RESOURCE_STATE_RENDER_TARGET, error), "initialize");
            const auto pass = [&](UINT region, const IndirectSignature& signature, UINT maxCount, VkDeviceSize argumentOffset, const VkDeviceSize* countOffset) {
                check(recorder.ClearRenderTarget(attachment, blue, {}, error), "clear");
                check(recorder.BeginRendering({attachment}, nullptr, {64, 64}, 1, error), "begin rendering");
                check(recorder.SetViewports({{0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f}}, error), "viewport");
                check(recorder.SetScissors({{0, 0, 64, 64}}, error), "scissor");
                check(recorder.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, false, error), "topology");
                recorder.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, graphics);
                if (signature.kind == IndirectKind::DrawIndexed) check(recorder.SetIndexBuffer({host.buffer, indices, 6}, DXGI_FORMAT_R16_UINT, error), "index buffer");
                check(recorder.ExecuteIndirect(signature, maxCount, hostRef, argumentOffset, countOffset != nullptr ? &hostRef : nullptr, countOffset != nullptr ? *countOffset : 0, error),
                      "execute indirect");
                recorder.EndRendering();
                check(recorder.Transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE, error), "to copy source");
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = prints->layouts[0];
                footprint.Offset = region * regionBytes;
                check(recorder.CopyTextureToBuffer(hostRef, footprint, target, D3D12_RESOURCE_STATE_COPY_SOURCE, 0, nullptr, error), "read back");
                check(recorder.Transition(target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET, error), "to render target");
            };
            pass(0, drawSignature, 2, drawArgs, nullptr);
            if (counted) {
                pass(1, drawSignature, 2, drawArgs, &countOne);
                pass(3, drawSignature, 2, drawArgs, &countZero);
            }
            pass(2, indexedSignature, 1, indexedArgs, nullptr);
            check(!recorder.ExecuteIndirect(dispatchSignature, 1, hostRef, dispatchArgs, &hostRef, countOne, error), "dispatch with a count is refused");
            check(!recorder.ExecuteIndirect(drawSignature, 2, hostRef, hostBytes - 40, nullptr, 0, error), "arguments past the buffer are refused");
            check(!recorder.ExecuteIndirect(drawSignature, 1, hostRef, drawArgs + 2, nullptr, 0, error), "misaligned arguments are refused");
            struct {
                std::uint64_t address;
                std::uint32_t base;
                std::uint32_t count;
            } constants{address + fillAt, 500, 256};
            recorder.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, compute);
            check(recorder.SetPushConstants(&constants, sizeof(constants), error), "push constants");
            check(recorder.ExecuteIndirect(dispatchSignature, 1, hostRef, dispatchArgs, nullptr, 0, error), "indirect dispatch");
            VkMemoryBarrier2 hostBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            hostBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            hostBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            hostBarrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            hostBarrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &hostBarrier;
            vk.cmdPipelineBarrier2(commands, &dependency);
        },
        error);
    int status = 0;
    if (!submitted || !recorded) status = Fail("recording '" + step + "': " + error);
    const auto rgb = [&](UINT region, UINT x, UINT y) {
        const std::uint8_t* p = bytes + region * regionBytes + y * pitch + x * 4;
        return static_cast<std::uint32_t>(p[0]) << 16 | static_cast<std::uint32_t>(p[1]) << 8 | p[2];
    };
    std::size_t checks = 0;
    const auto expect = [&](bool ok, const std::string& what) {
        ++checks;
        if (!ok && status == 0) status = Fail(what);
    };
    if (status == 0) {
        const std::uint32_t red = 0xFF0000u;
        const std::uint32_t blueRgb = 0x0000FFu;
        expect(rgb(0, 4, 4) == red && rgb(0, 60, 60) == red, "two indirect draws cover both halves");
        if (counted) {
            expect(rgb(1, 4, 4) == red && rgb(1, 60, 60) == blueRgb, "a count of 1 draws only the first command");
            expect(rgb(3, 4, 4) == blueRgb && rgb(3, 60, 60) == blueRgb, "a count of 0 draws nothing");
        }
        expect(rgb(2, 60, 60) == red && rgb(2, 4, 4) == blueRgb, "the indexed indirect draw covers the lower-right half");
        bool filled = true;
        for (std::uint32_t item = 0; item < 128 && filled; ++item) {
            std::uint32_t value = 0;
            std::memcpy(&value, bytes + fillAt + item * 4, sizeof(value));
            filled = value == 500 + item * 3;
        }
        std::uint32_t beyond = 1;
        std::memcpy(&beyond, bytes + fillAt + 128 * 4, sizeof(beyond));
        expect(filled && beyond == 0, "the indirect dispatch ran 2 groups of 64 and no more");
    }
    std::printf("indirect: %zu read-back checks\n", checks);
    vk.destroyImageView(device->Handle(), targetView, nullptr);
    vk.destroyImage(device->Handle(), target.image, nullptr);
    vk.freeMemory(device->Handle(), targetMemory, nullptr);
    DestroyBuffer(*device, host);
    for (VkPipeline pipeline : {graphics, compute}) vk.destroyPipeline(device->Handle(), pipeline, nullptr);
    for (VkShaderModule module : {quadrant, fragment, fill}) vk.destroyShaderModule(device->Handle(), module, nullptr);
    if (validate) {
        std::printf("validation errors=%zu\n", device->ValidationErrors());
        for (const std::string& message : device->ValidationMessages()) std::printf("  %.600s\n", message.c_str());
        if (device->ValidationErrors() != 0) status = 1;
    }
    if (status == 0) std::puts("PASS indirect gpu");
    return status;
}

int PlanesGpu(bool validate) {
    DeviceRequirements requirements;
    requirements.validation = validate;
    std::string error;
    const std::unique_ptr<VulkanDevice> device = VulkanDevice::Create(requirements, error);
    if (!device) {
        std::printf("SKIP no usable Vulkan device: %s\n", error.c_str());
        return validate && error == "the Khronos validation layer is not installed" ? kNoValidation : kNoDevice;
    }
    const DeviceFunctions& vk = device->Vk();
    struct Owned {
        TextureRef ref;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    std::vector<Owned> owned;
    owned.reserve(4);
    const auto make = [&](const D3D12_RESOURCE_DESC& desc, bool depthView) -> Owned* {
        const std::optional<TextureImage> image = TranslateTextureDesc(desc, error);
        if (!image) return nullptr;
        Owned texture;
        texture.ref.desc = desc;
        texture.ref.layout = *image;
        if (vk.createImage(device->Handle(), &image->info, nullptr, &texture.ref.image) != VK_SUCCESS) return nullptr;
        VkMemoryRequirements needs{};
        vk.getImageMemoryRequirements(device->Handle(), texture.ref.image, &needs);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = device->MemoryType(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT).value_or(0);
        if (vk.allocateMemory(device->Handle(), &allocate, nullptr, &texture.memory) != VK_SUCCESS || vk.bindImageMemory(device->Handle(), texture.ref.image, texture.memory, 0) != VK_SUCCESS) return nullptr;
        std::optional<ImageView> view = depthView ? DepthStencilView(desc, nullptr, error) : RenderTargetView(desc, nullptr, error);
        if (!view) return nullptr;
        view->info.image = texture.ref.image;
        if (vk.createImageView(device->Handle(), &view->info, nullptr, &texture.view) != VK_SUCCESS) return nullptr;
        owned.push_back(texture);
        return &owned.back();
    };
    const D3D12_RESOURCE_FLAGS depthFlags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    Owned* depth = make(TextureDesc2D(DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 16, 16, 1, 1, depthFlags), true);
    Owned* copy = make(TextureDesc2D(DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 16, 16, 1, 1, depthFlags), true);
    Owned* color = make(TextureDesc2D(DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET), false);
    if (!depth || !copy || !color) return Fail("textures: " + error);
    const VkShaderModuleCreateInfo cornerInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sizeof(kCornerVertex), kCornerVertex};
    const VkShaderModuleCreateInfo fragmentInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sizeof(kGraphicsFloat), kGraphicsFloat};
    VkShaderModule corner = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    if (vk.createShaderModule(device->Handle(), &cornerInfo, nullptr, &corner) != VK_SUCCESS || vk.createShaderModule(device->Handle(), &fragmentInfo, nullptr, &fragment) != VK_SUCCESS) return Fail("modules");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = DefaultGraphics();
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    const std::optional<GraphicsPipelineState> passing = TranslateGraphicsState(desc, error);
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
    const std::optional<GraphicsPipelineState> failing = TranslateGraphicsState(desc, error);
    VkPipeline passes = VK_NULL_HANDLE;
    VkPipeline fails = VK_NULL_HANDLE;
    if (!passing || !failing ||
        CreateGraphicsPipeline(*device, VK_NULL_HANDLE, *passing, {{VK_SHADER_STAGE_VERTEX_BIT, corner, 0}, {VK_SHADER_STAGE_FRAGMENT_BIT, fragment, 0}}, {}, 0, &passes, error) != VK_SUCCESS ||
        CreateGraphicsPipeline(*device, VK_NULL_HANDLE, *failing, {{VK_SHADER_STAGE_VERTEX_BIT, corner, 0}, {VK_SHADER_STAGE_FRAGMENT_BIT, fragment, 0}}, {}, 0, &fails, error) != VK_SUCCESS) {
        return Fail("pipelines: " + error);
    }
    DeviceBuffer host;
    const VkDeviceSize hostBytes = 1u << 20;
    if (!CreateHostBuffer(*device, hostBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, host, error)) return Fail("host buffer: " + error);
    std::uint8_t* const bytes = static_cast<std::uint8_t*>(host.mapped);
    std::fill(bytes, bytes + hostBytes, std::uint8_t{0xEE});
    const BufferRef hostRef{host.buffer, 0, hostBytes};
    const auto footprint = [](UINT64 offset, DXGI_FORMAT format, UINT width) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT print{};
        print.Offset = offset;
        print.Footprint = {format, width, 16, 1, 256};
        return print;
    };
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT depthPlane = footprint(0x0000, DXGI_FORMAT_R32_TYPELESS, 16);
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT stencilPlane = footprint(0x2000, DXGI_FORMAT_R8_TYPELESS, 16);
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT copiedStencil = footprint(0x4000, DXGI_FORMAT_R8_TYPELESS, 16);
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT passed = footprint(0x6000, DXGI_FORMAT_R8G8B8A8_UNORM, 16);
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT failed = footprint(0x8000, DXGI_FORMAT_R8G8B8A8_UNORM, 16);
    const AttachmentRef depthTarget{depth->view, {16, 16}, 1, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT};
    const AttachmentRef copyTarget{copy->view, {16, 16}, 1, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT};
    const AttachmentRef readOnlyDepth{depth->view, {16, 16}, 1, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                                      static_cast<D3D12_DSV_FLAGS>(D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL)};
    const AttachmentRef halfReadOnly{depth->view, {16, 16}, 1, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, D3D12_DSV_FLAG_READ_ONLY_DEPTH};
    const AttachmentRef colorTarget{color->view, {16, 16}, 1, VK_IMAGE_ASPECT_COLOR_BIT};
    bool recorded = true;
    std::string step;
    const bool submitted = device->Submit(
        [&](VkCommandBuffer commands) {
            CommandRecorder recorder(*device, commands);
            const auto check = [&](bool ok, const char* name) {
                if (!ok && recorded) {
                    recorded = false;
                    step = name;
                }
            };
            const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            check(recorder.InitializeTexture(depth->ref, D3D12_RESOURCE_STATE_DEPTH_WRITE, error), "initialize depth");
            check(recorder.InitializeTexture(copy->ref, D3D12_RESOURCE_STATE_DEPTH_WRITE, error), "initialize copy");
            check(recorder.InitializeTexture(color->ref, D3D12_RESOURCE_STATE_RENDER_TARGET, error), "initialize color");
            check(recorder.ClearDepthStencil(depthTarget, static_cast<D3D12_CLEAR_FLAGS>(D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL), 0.5f, 0x5A, {}, error), "clear both");
            check(recorder.ClearDepthStencil(depthTarget, D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0x33, {{0, 0, 4, 4}}, error), "clear stencil rect");
            check(recorder.ClearDepthStencil(copyTarget, static_cast<D3D12_CLEAR_FLAGS>(D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL), 1.0f, 0x00, {}, error), "clear copy");
            check(recorder.Transition(depth->ref, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE, error), "depth to copy source");
            check(recorder.Transition(copy->ref, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_DEST, error), "copy to copy dest");
            check(recorder.CopyTextureToBuffer(hostRef, depthPlane, depth->ref, D3D12_RESOURCE_STATE_COPY_SOURCE, 0, nullptr, error), "read depth plane");
            check(recorder.CopyTextureToBuffer(hostRef, stencilPlane, depth->ref, D3D12_RESOURCE_STATE_COPY_SOURCE, 1, nullptr, error), "read stencil plane");
            check(recorder.CopyTexture(copy->ref, D3D12_RESOURCE_STATE_COPY_DEST, 1, 0, 0, 0, depth->ref, D3D12_RESOURCE_STATE_COPY_SOURCE, 1, nullptr, error), "copy stencil plane");
            check(!recorder.CopyTexture(copy->ref, D3D12_RESOURCE_STATE_COPY_DEST, 0, 0, 0, 0, depth->ref, D3D12_RESOURCE_STATE_COPY_SOURCE, 1, nullptr, error), "mixed planes refused");
            check(!recorder.CopyTextureToBuffer(hostRef, depthPlane, depth->ref, D3D12_RESOURCE_STATE_COPY_SOURCE, 1, nullptr, error), "4-byte stencil footprint refused");
            check(recorder.Transition(copy->ref, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE, error), "copy to copy source");
            check(recorder.CopyTextureToBuffer(hostRef, copiedStencil, copy->ref, D3D12_RESOURCE_STATE_COPY_SOURCE, 1, nullptr, error), "read copied stencil");
            check(recorder.Transition(depth->ref, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                      static_cast<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE), error), "depth to read");
            const auto draw = [&](VkPipeline pipeline, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& print) {
                check(recorder.ClearRenderTarget(colorTarget, blue, {}, error), "clear color");
                check(recorder.BeginRendering({colorTarget}, &readOnlyDepth, {16, 16}, 1, error), "read-only pass");
                check(recorder.SetViewports({{0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 1.0f}}, error), "viewport");
                check(recorder.SetScissors({{0, 0, 16, 16}}, error), "scissor");
                check(recorder.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, false, error), "topology");
                recorder.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                recorder.Draw(3, 1, 0, 0);
                recorder.EndRendering();
                check(recorder.Transition(color->ref, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE, error), "color to copy source");
                check(recorder.CopyTextureToBuffer(hostRef, print, color->ref, D3D12_RESOURCE_STATE_COPY_SOURCE, 0, nullptr, error), "read color");
                check(recorder.Transition(color->ref, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET, error), "color to render target");
            };
            draw(passes, passed);
            draw(fails, failed);
            check(!recorder.BeginRendering({colorTarget}, &halfReadOnly, {16, 16}, 1, error), "half read-only refused");
            VkMemoryBarrier2 hostBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            hostBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            hostBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            hostBarrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            hostBarrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &hostBarrier;
            vk.cmdPipelineBarrier2(commands, &dependency);
        },
        error);
    int status = 0;
    if (!submitted || !recorded) status = Fail("recording '" + step + "': " + error);
    std::size_t checks = 0;
    const auto expect = [&](bool ok, const std::string& what) {
        ++checks;
        if (!ok && status == 0) status = Fail(what);
    };
    if (status == 0) {
        float depthValue = 0.0f;
        std::memcpy(&depthValue, bytes + depthPlane.Offset + 8 * 256 + 8 * 4, sizeof(depthValue));
        expect(depthValue == 0.5f, "depth plane read back as " + std::to_string(depthValue));
        expect(bytes[stencilPlane.Offset + 1 * 256 + 1] == 0x33 && bytes[stencilPlane.Offset + 8 * 256 + 8] == 0x5A, "stencil plane read back with the rectangle");
        expect(bytes[copiedStencil.Offset + 2 * 256 + 2] == 0x33 && bytes[copiedStencil.Offset + 12 * 256 + 12] == 0x5A, "stencil plane copied between textures");
        const auto rgb = [&](const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& print, UINT x, UINT y) {
            const std::uint8_t* p = bytes + print.Offset + y * 256 + x * 4;
            return static_cast<std::uint32_t>(p[0]) << 16 | static_cast<std::uint32_t>(p[1]) << 8 | p[2];
        };
        expect(rgb(passed, 2, 2) == 0xFF0000u && rgb(passed, 14, 14) == 0x0000FFu, "a read-only pass draws where depth 0 <= 0.5");
        expect(rgb(failed, 2, 2) == 0x0000FFu, "a read-only pass draws nothing where depth 0 > 0.5 fails");
    }
    std::printf("planes: %zu read-back checks\n", checks);
    for (Owned& texture : owned) {
        vk.destroyImageView(device->Handle(), texture.view, nullptr);
        vk.destroyImage(device->Handle(), texture.ref.image, nullptr);
        vk.freeMemory(device->Handle(), texture.memory, nullptr);
    }
    DestroyBuffer(*device, host);
    vk.destroyPipeline(device->Handle(), passes, nullptr);
    vk.destroyPipeline(device->Handle(), fails, nullptr);
    vk.destroyShaderModule(device->Handle(), corner, nullptr);
    vk.destroyShaderModule(device->Handle(), fragment, nullptr);
    if (validate) {
        std::printf("validation errors=%zu\n", device->ValidationErrors());
        for (const std::string& message : device->ValidationMessages()) std::printf("  %.600s\n", message.c_str());
        if (device->ValidationErrors() != 0) status = 1;
    }
    if (status == 0) std::puts("PASS planes gpu");
    return status;
}

int BuffersSelfTest() {
    std::string error;
    BufferDescriptorFields fields;
    fields.base = 0x123456789ABCull;
    fields.stride = 48;
    fields.records = 1000;
    fields.format = 77;
    fields.bounds = BufferBounds::Raw;
    const std::optional<BufferDescriptorWords> words = EncodeBufferDescriptor(fields, error);
    if (!words || (*words)[0] != 0x56789ABCu || (*words)[1] != 0x00301234u || (*words)[2] != 1000u || (*words)[3] != 0x3104DFACu) return Fail("encoded words");
    const BufferDescriptorFields decoded = DecodeBufferDescriptor(*words);
    if (decoded.base != fields.base || decoded.stride != 48 || decoded.records != 1000 || decoded.format != 77 || decoded.bounds != BufferBounds::Raw || decoded.select != fields.select) return Fail("decode");
    const BufferDescriptorWords translatorTest{0, 0, 0, 0x31016FACu};
    const BufferDescriptorFields reference = DecodeBufferDescriptor(translatorTest);
    BufferDescriptorFields same;
    same.format = 22;
    const std::optional<BufferDescriptorWords> sameWords = EncodeBufferDescriptor(same, error);
    if (reference.bounds != BufferBounds::Raw || reference.format != 22 || !sameWords || (*sameWords)[3] != 0x31016FACu) return Fail("the translator's reference word 3");
    fields.base = kDescriptorAddressLimit;
    if (EncodeBufferDescriptor(fields, error) || error.find("48 bits") == std::string::npos) return Fail("address limit");
    fields.base = 0;
    fields.stride = kDescriptorStrideLimit + 1;
    if (EncodeBufferDescriptor(fields, error)) return Fail("stride limit");
    if (TypedBufferFormatOf(DXGI_FORMAT_R11G11B10_FLOAT)->code != 36 || TypedBufferFormatOf(DXGI_FORMAT_R10G10B10A2_UNORM)->code != 50 ||
        TypedBufferFormatOf(DXGI_FORMAT_R32G32B32_FLOAT)->bytes != 12 || TypedBufferFormatOf(DXGI_FORMAT_BC1_UNORM) || TypedBufferFormatOf(DXGI_FORMAT_R32_TYPELESS)) return Fail("typed formats");

    const auto view = [](const BufferDescriptorWords& w) { return DecodeBufferDescriptor(w); };
    const std::optional<BufferDescriptorWords> cbv = ConstantBufferDescriptor({0x10000, 256}, error);
    if (!cbv || view(*cbv).base != 0x10000 || view(*cbv).records != 256 || view(*cbv).bounds != BufferBounds::Raw || view(*cbv).stride != 0) return Fail("constant buffer: " + error);
    if (ConstantBufferDescriptor({0x10000, 100}, error) || ConstantBufferDescriptor({0, 256}, error)) return Fail("constant buffer refusals");

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.Buffer = {16, 64, 0, D3D12_BUFFER_SRV_FLAG_RAW};
    std::optional<BufferDescriptorWords> raw = ShaderResourceBufferDescriptor(0x20000, 1024, srv, error);
    if (!raw || view(*raw).base != 0x20040 || view(*raw).records != 256 || view(*raw).bounds != BufferBounds::Raw || view(*raw).stride != 0) return Fail("raw view: " + error);
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.Buffer = {2, 10, 32, D3D12_BUFFER_SRV_FLAG_NONE};
    std::optional<BufferDescriptorWords> structured = ShaderResourceBufferDescriptor(0x20000, 1024, srv, error);
    if (!structured || view(*structured).base != 0x20040 || view(*structured).records != 10 || view(*structured).stride != 32 ||
        view(*structured).bounds != BufferBounds::StructuredWithOffset) return Fail("structured view: " + error);
    srv.Format = DXGI_FORMAT_R16G16_FLOAT;
    srv.Buffer = {4, 8, 0, D3D12_BUFFER_SRV_FLAG_NONE};
    std::optional<BufferDescriptorWords> typed = ShaderResourceBufferDescriptor(0x20000, 1024, srv, error);
    const std::array<std::uint32_t, 4> twoComponents{SelectX, SelectY, SelectZero, SelectOne};
    if (!typed || view(*typed).base != 0x20010 || view(*typed).records != 8 || view(*typed).stride != 4 || view(*typed).format != 29 ||
        view(*typed).bounds != BufferBounds::Structured || view(*typed).select != twoComponents || (*typed)[3] != 0x1101D22Cu) return Fail("typed view: " + error);
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(1, 0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0);
    typed = ShaderResourceBufferDescriptor(0x20000, 1024, srv, error);
    const std::array<std::uint32_t, 4> mapped{SelectZero, SelectX, SelectOne, SelectZero};
    if (!typed || view(*typed).select != mapped) return Fail("mapped typed view");

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Format = DXGI_FORMAT_R32_UINT;
    uav.Buffer = {0, 256, 0, 0, D3D12_BUFFER_UAV_FLAG_NONE};
    std::optional<BufferDescriptorWords> storage = UnorderedAccessBufferDescriptor(0x30000, 1024, uav, false, error);
    if (!storage || view(*storage).records != 256 || view(*storage).format != 20 || view(*storage).bounds != BufferBounds::Structured) return Fail("typed uav: " + error);

    struct Refusal {
        std::optional<BufferDescriptorWords> (*make)(std::string&);
        const char* expected;
    };
    const Refusal refusals[] = {
        {[](std::string& e) {
             D3D12_SHADER_RESOURCE_VIEW_DESC d{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, {}};
             d.Buffer = {0, 300, 0, D3D12_BUFFER_SRV_FLAG_NONE};
             return ShaderResourceBufferDescriptor(0x1000, 1024, d, e);
         }, "elements 0+300 run past the buffer's 256"},
        {[](std::string& e) {
             D3D12_SHADER_RESOURCE_VIEW_DESC d{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, {}};
             d.Buffer = {0, 4, 16, D3D12_BUFFER_SRV_FLAG_NONE};
             return ShaderResourceBufferDescriptor(0x1000, 1024, d, e);
         }, "must have format UNKNOWN"},
        {[](std::string& e) {
             D3D12_SHADER_RESOURCE_VIEW_DESC d{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, {}};
             d.Buffer = {0, 4, 0, D3D12_BUFFER_SRV_FLAG_RAW};
             return ShaderResourceBufferDescriptor(0x1000, 1024, d, e);
         }, "needs R32_TYPELESS"},
        {[](std::string& e) {
             D3D12_SHADER_RESOURCE_VIEW_DESC d{DXGI_FORMAT_UNKNOWN, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, {}};
             d.Buffer = {0, 4, 18, D3D12_BUFFER_SRV_FLAG_NONE};
             return ShaderResourceBufferDescriptor(0x1000, 1024, d, e);
         }, "structure stride 18"},
        {[](std::string& e) {
             D3D12_SHADER_RESOURCE_VIEW_DESC d{DXGI_FORMAT_BC1_UNORM, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, {}};
             d.Buffer = {0, 4, 0, D3D12_BUFFER_SRV_FLAG_NONE};
             return ShaderResourceBufferDescriptor(0x1000, 1024, d, e);
         }, "has no typed buffer encoding"},
        {[](std::string& e) {
             D3D12_SHADER_RESOURCE_VIEW_DESC d{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, {}};
             return ShaderResourceBufferDescriptor(0x1000, 1024, d, e);
         }, "dimension 4 is not a buffer"},
        {[](std::string& e) {
             D3D12_SHADER_RESOURCE_VIEW_DESC d{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, {}};
             d.Buffer = {0, 0, 0, D3D12_BUFFER_SRV_FLAG_NONE};
             return ShaderResourceBufferDescriptor(0x1000, 1024, d, e);
         }, "has no elements"},
        {[](std::string& e) {
             D3D12_SHADER_RESOURCE_VIEW_DESC d{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, {}};
             d.Buffer = {0, 4, 0, static_cast<D3D12_BUFFER_SRV_FLAGS>(2)};
             return ShaderResourceBufferDescriptor(0x1000, 1024, d, e);
         }, "flags 2 are unknown"},
        {[](std::string& e) {
             D3D12_UNORDERED_ACCESS_VIEW_DESC d{DXGI_FORMAT_R32_UINT, D3D12_UAV_DIMENSION_BUFFER, {}};
             d.Buffer = {0, 4, 0, 0, D3D12_BUFFER_UAV_FLAG_NONE};
             return UnorderedAccessBufferDescriptor(0x1000, 1024, d, true, e);
         }, "with a counter are not supported"},
        {[](std::string& e) {
             D3D12_SHADER_RESOURCE_VIEW_DESC d{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, {}};
             d.Buffer = {0, 4, 0, D3D12_BUFFER_SRV_FLAG_NONE};
             return ShaderResourceBufferDescriptor(kDescriptorAddressLimit, 1024, d, e);
         }, "does not fit the descriptor's 48 bits"},
    };
    for (const Refusal& refusal : refusals) {
        error.clear();
        if (refusal.make(error) || error.find(refusal.expected) == std::string::npos) return Fail(std::string("refusal '") + refusal.expected + "' gave '" + error + "'");
    }
    std::puts("PASS buffers selftest");
    return 0;
}

int RootSignatureSelfTest() {
    std::string error;
    const D3D12_DESCRIPTOR_RANGE1 viewRanges[] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 10, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 1, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0xFFFFFFFFu, 0, 1, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
    };
    const D3D12_DESCRIPTOR_RANGE1 samplerRange{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 4, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
    D3D12_ROOT_PARAMETER1 parameters[4]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 4};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[1].Descriptor = {1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable = {3, viewRanges};
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[3].DescriptorTable = {1, &samplerRange};
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC statics[2]{};
    statics[0] = {D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0.0f, 1,
                  D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK, 0.0f, D3D12_FLOAT32_MAX, 0, 1, D3D12_SHADER_VISIBILITY_PIXEL};
    statics[1] = {D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER, 0.0f, 1,
                  D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE, 0.0f, D3D12_FLOAT32_MAX, 1, 1, D3D12_SHADER_VISIBILITY_ALL};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1 = {4, parameters, 2, statics, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    const auto check = [&](const RootSignatureLayout& layout, const char* where) -> bool {
        if (layout.words != 8 || layout.parameters.size() != 4 || layout.staticSamplers.size() != 2 ||
            layout.flags != D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT) return Fail(std::string(where) + ": counts") == 0;
        const RootParameterLayout& constants = layout.parameters[0];
        const RootParameterLayout& cbv = layout.parameters[1];
        const RootParameterLayout& table = layout.parameters[2];
        const RootParameterLayout& samplerTable = layout.parameters[3];
        if (constants.words != 4 || constants.firstWord != 0 || constants.constants != 4) return Fail(std::string(where) + ": constants") == 0;
        if (cbv.words != 2 || cbv.firstWord != 4 || cbv.shaderRegister != 1 || cbv.visibility != D3D12_SHADER_VISIBILITY_VERTEX) return Fail(std::string(where) + ": root CBV") == 0;
        if (table.words != 1 || table.firstWord != 6 || table.ranges.size() != 3 || table.ranges[1].RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_UAV || table.ranges[2].NumDescriptors != 0xFFFFFFFFu ||
            table.ranges[2].RegisterSpace != 1) return Fail(std::string(where) + ": view table") == 0;
        if (samplerTable.firstWord != 7 || samplerTable.ranges[0].RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER || samplerTable.ranges[0].NumDescriptors != 4) return Fail(std::string(where) + ": sampler table") == 0;
        const Sampler& shadow = layout.staticSamplers[1].sampler;
        if (!shadow.info.compareEnable || shadow.info.borderColor != VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE || layout.staticSamplers[1].desc.ShaderRegister != 1 ||
            layout.staticSamplers[0].sampler.info.minFilter != VK_FILTER_LINEAR) return Fail(std::string(where) + ": static samplers") == 0;
        return true;
    };
    const std::optional<RootSignatureLayout> built = BuildRootSignatureLayout(desc, error);
    if (!built) return Fail("build: " + error);
    if (!check(*built, "built")) return 1;

    const HMODULE host = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto serialize = host ? reinterpret_cast<PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE>(reinterpret_cast<void*>(GetProcAddress(host, "D3D12SerializeVersionedRootSignature"))) : nullptr;
    const auto serializeOld = host ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(reinterpret_cast<void*>(GetProcAddress(host, "D3D12SerializeRootSignature"))) : nullptr;
    if (serialize == nullptr || serializeOld == nullptr) {
        std::puts("SKIP the host has no d3d12.dll serializer; round trip not checked");
    } else {
        ID3DBlob* blob = nullptr;
        ID3DBlob* messages = nullptr;
        if (FAILED(serialize(&desc, &blob, &messages))) return Fail(std::string("host serialize 1.1: ") + (messages ? static_cast<const char*>(messages->GetBufferPointer()) : ""));
        const std::optional<RootSignatureLayout> parsed = DeserializeRootSignature(blob->GetBufferPointer(), blob->GetBufferSize(), error);
        blob->Release();
        if (!parsed) return Fail("deserialize 1.1: " + error);
        if (!check(*parsed, "round trip 1.1")) return 1;
        const D3D12_DESCRIPTOR_RANGE oldRange{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 3, 2, 0, 0};
        D3D12_ROOT_PARAMETER oldParameters[2]{};
        oldParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        oldParameters[0].DescriptorTable = {1, &oldRange};
        oldParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        oldParameters[1].Descriptor = {5, 2};
        const D3D12_ROOT_SIGNATURE_DESC old{2, oldParameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        if (FAILED(serializeOld(&old, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &messages))) return Fail("host serialize 1.0");
        const std::optional<RootSignatureLayout> converted = DeserializeRootSignature(blob->GetBufferPointer(), blob->GetBufferSize(), error);
        blob->Release();
        if (!converted || converted->words != 3 || converted->parameters[0].ranges[0].RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_CBV ||
            converted->parameters[0].ranges[0].BaseShaderRegister != 2 || converted->parameters[1].shaderRegister != 5 || converted->parameters[1].registerSpace != 2) {
            return Fail("version 1.0 converted to 1.1: " + error);
        }
        const unsigned char junk[16] = {1, 2, 3, 4};
        if (DeserializeRootSignature(junk, sizeof(junk), error) || error.find("rejected") == std::string::npos) return Fail("junk blob");
        std::puts("host round trip: 1.1 and 1.0 blobs deserialized");
    }

    D3D12_DESCRIPTOR_RANGE1 mixed[2] = {viewRanges[0], samplerRange};
    D3D12_DESCRIPTOR_RANGE1 unboundedFirst[2] = {viewRanges[2], viewRanges[0]};
    struct Refusal {
        void (*edit)(D3D12_VERSIONED_ROOT_SIGNATURE_DESC&, D3D12_ROOT_PARAMETER1*, D3D12_STATIC_SAMPLER_DESC*, D3D12_DESCRIPTOR_RANGE1*, D3D12_DESCRIPTOR_RANGE1*);
        const char* expected;
    };
    const Refusal refusals[] = {
        {[](D3D12_VERSIONED_ROOT_SIGNATURE_DESC&, D3D12_ROOT_PARAMETER1* p, D3D12_STATIC_SAMPLER_DESC*, D3D12_DESCRIPTOR_RANGE1*, D3D12_DESCRIPTOR_RANGE1*) { p[0].Constants.Num32BitValues = 61; },
         "more than 64 32-bit words"},
        {[](D3D12_VERSIONED_ROOT_SIGNATURE_DESC&, D3D12_ROOT_PARAMETER1* p, D3D12_STATIC_SAMPLER_DESC*, D3D12_DESCRIPTOR_RANGE1* m, D3D12_DESCRIPTOR_RANGE1*) { p[2].DescriptorTable = {2, m}; },
         "mixes samplers with other descriptors"},
        {[](D3D12_VERSIONED_ROOT_SIGNATURE_DESC&, D3D12_ROOT_PARAMETER1* p, D3D12_STATIC_SAMPLER_DESC*, D3D12_DESCRIPTOR_RANGE1*, D3D12_DESCRIPTOR_RANGE1* u) { p[2].DescriptorTable = {2, u}; },
         "unbounded range before its last range"},
        {[](D3D12_VERSIONED_ROOT_SIGNATURE_DESC&, D3D12_ROOT_PARAMETER1* p, D3D12_STATIC_SAMPLER_DESC*, D3D12_DESCRIPTOR_RANGE1*, D3D12_DESCRIPTOR_RANGE1*) { p[2].DescriptorTable = {0, nullptr}; },
         "has no ranges"},
        {[](D3D12_VERSIONED_ROOT_SIGNATURE_DESC&, D3D12_ROOT_PARAMETER1* p, D3D12_STATIC_SAMPLER_DESC*, D3D12_DESCRIPTOR_RANGE1*, D3D12_DESCRIPTOR_RANGE1*) { p[0].Constants.Num32BitValues = 0; },
         "have no values"},
        {[](D3D12_VERSIONED_ROOT_SIGNATURE_DESC&, D3D12_ROOT_PARAMETER1* p, D3D12_STATIC_SAMPLER_DESC*, D3D12_DESCRIPTOR_RANGE1*, D3D12_DESCRIPTOR_RANGE1*) { p[1].ShaderVisibility = static_cast<D3D12_SHADER_VISIBILITY>(9); },
         "shader visibility 9"},
        {[](D3D12_VERSIONED_ROOT_SIGNATURE_DESC&, D3D12_ROOT_PARAMETER1*, D3D12_STATIC_SAMPLER_DESC* s, D3D12_DESCRIPTOR_RANGE1*, D3D12_DESCRIPTOR_RANGE1*) { s[1].BorderColor = static_cast<D3D12_STATIC_BORDER_COLOR>(3); },
         "static border color 3"},
        {[](D3D12_VERSIONED_ROOT_SIGNATURE_DESC& d, D3D12_ROOT_PARAMETER1*, D3D12_STATIC_SAMPLER_DESC*, D3D12_DESCRIPTOR_RANGE1*, D3D12_DESCRIPTOR_RANGE1*) { d.Desc_1_1.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(0x03000000); },
         "flags 50331648 are unknown"},
        {[](D3D12_VERSIONED_ROOT_SIGNATURE_DESC& d, D3D12_ROOT_PARAMETER1*, D3D12_STATIC_SAMPLER_DESC*, D3D12_DESCRIPTOR_RANGE1*, D3D12_DESCRIPTOR_RANGE1*) { d.Version = static_cast<D3D_ROOT_SIGNATURE_VERSION>(5); },
         "version 5 is not 1.0 or 1.1"},
    };
    for (const Refusal& refusal : refusals) {
        D3D12_ROOT_PARAMETER1 copy[4];
        std::copy(std::begin(parameters), std::end(parameters), copy);
        D3D12_STATIC_SAMPLER_DESC staticCopy[2] = {statics[0], statics[1]};
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC edited = desc;
        edited.Desc_1_1.pParameters = copy;
        edited.Desc_1_1.pStaticSamplers = staticCopy;
        refusal.edit(edited, copy, staticCopy, mixed, unboundedFirst);
        error.clear();
        if (BuildRootSignatureLayout(edited, error) || error.find(refusal.expected) == std::string::npos) return Fail(std::string("refusal '") + refusal.expected + "' gave '" + error + "'");
    }
    std::puts("PASS root signature selftest");
    return 0;
}

int FenceGpu(bool validate) {
    DeviceRequirements requirements;
    requirements.validation = validate;
    std::string error;
    const std::unique_ptr<VulkanDevice> device = VulkanDevice::Create(requirements, error);
    if (!device) {
        std::printf("SKIP no usable Vulkan device: %s\n", error.c_str());
        return validate && error == "the Khronos validation layer is not installed" ? kNoValidation : kNoDevice;
    }
    int status = 0;
    std::size_t checks = 0;
    const auto expect = [&](bool ok, const std::string& what) {
        ++checks;
        if (!ok && status == 0) status = Fail(what);
    };
    ID3D12Fence* fence = nullptr;
    ID3D12Fence* other = nullptr;
    expect(SUCCEEDED(Fence::Create(*device, nullptr, 0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence), error)) && fence != nullptr, "create fence: " + error);
    expect(SUCCEEDED(Fence::Create(*device, nullptr, 0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&other), error)) && other != nullptr, "create second fence");
    void* nothing = nullptr;
    expect(Fence::Create(*device, nullptr, 0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), &nothing, error) == E_INVALIDARG && nothing == nullptr, "shared fence refused");
    D3D12_COMMAND_QUEUE_DESC directDesc{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    D3D12_COMMAND_QUEUE_DESC computeDesc{D3D12_COMMAND_LIST_TYPE_COMPUTE, 0, D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT, 0};
    ID3D12CommandQueue* direct = nullptr;
    ID3D12CommandQueue* compute = nullptr;
    expect(SUCCEEDED(CommandQueue::Create(*device, nullptr, directDesc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&direct), error)) && direct != nullptr, "create direct queue");
    expect(SUCCEEDED(CommandQueue::Create(*device, nullptr, computeDesc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&compute), error)) && compute != nullptr, "create compute queue");
    D3D12_COMMAND_QUEUE_DESC videoDesc{D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    expect(CommandQueue::Create(*device, nullptr, videoDesc, __uuidof(ID3D12CommandQueue), &nothing, error) == E_INVALIDARG, "video queue refused");
    if (status != 0) return status;

    void* queried = nullptr;
    for (const IID& iid : {__uuidof(IUnknown), __uuidof(ID3D12Object), __uuidof(ID3D12DeviceChild), __uuidof(ID3D12Pageable), __uuidof(ID3D12Fence), __uuidof(ID3D12Fence1)}) {
        queried = nullptr;
        const HRESULT result = fence->QueryInterface(iid, &queried);
        expect(SUCCEEDED(result) && queried == fence, "fence interface");
        if (queried != nullptr) static_cast<IUnknown*>(queried)->Release();
    }
    expect(fence->QueryInterface(__uuidof(ID3D12CommandQueue), &queried) == E_NOINTERFACE && queried == nullptr, "fence is not a queue");
    expect(fence->GetDevice(__uuidof(ID3D12Device), &queried) == E_NOINTERFACE, "no parent device");
    const GUID tag{0x12345678, 0x1234, 0x5678, {1, 2, 3, 4, 5, 6, 7, 8}};
    const std::uint32_t payload = 0xC0FFEEu;
    UINT size = 0;
    expect(fence->GetPrivateData(tag, &size, nullptr) == DXGI_ERROR_NOT_FOUND, "missing private data");
    expect(SUCCEEDED(fence->SetPrivateData(tag, sizeof(payload), &payload)), "set private data");
    expect(SUCCEEDED(fence->GetPrivateData(tag, &size, nullptr)) && size == 4, "private data size");
    std::uint32_t read = 0;
    size = 2;
    expect(fence->GetPrivateData(tag, &size, &read) == DXGI_ERROR_MORE_DATA && size == 4, "short private data buffer");
    size = 4;
    expect(SUCCEEDED(fence->GetPrivateData(tag, &size, &read)) && read == payload, "private data value");
    expect(SUCCEEDED(fence->SetPrivateDataInterface(tag, other)), "private interface");
    IUnknown* stored = nullptr;
    size = sizeof(stored);
    expect(SUCCEEDED(fence->GetPrivateData(tag, &size, &stored)) && stored == other, "private interface value");
    if (stored != nullptr) stored->Release();
    expect(SUCCEEDED(fence->SetName(L"frame fence")), "set name");
    ID3D12Fence1* fence1 = nullptr;
    if (SUCCEEDED(fence->QueryInterface(__uuidof(ID3D12Fence1), reinterpret_cast<void**>(&fence1)))) {
        expect(fence1->GetCreationFlags() == D3D12_FENCE_FLAG_NONE, "creation flags");
        fence1->Release();
    }
    const D3D12_COMMAND_QUEUE_DESC readBack = compute->GetDesc();
    expect(readBack.Type == D3D12_COMMAND_LIST_TYPE_COMPUTE && readBack.Flags == D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT, "queue description");
    UINT64 frequency = 0;
    expect(SUCCEEDED(direct->GetTimestampFrequency(&frequency)) && frequency > 0, "timestamp frequency");
    expect(direct->Signal(nullptr, 1) == E_INVALIDARG, "foreign fence refused");

    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const auto reaches = [&](ID3D12Fence* which, UINT64 value) {
        ResetEvent(event);
        return SUCCEEDED(which->SetEventOnCompletion(value, event)) && WaitForSingleObject(event, 5000) == WAIT_OBJECT_0 && which->GetCompletedValue() >= value;
    };
    expect(SUCCEEDED(direct->Signal(fence, 1)), "queue signal");
    expect(reaches(fence, 1), "a GPU signal wakes the event");
    CommandQueue* directQueue = dynamic_cast<CommandQueue*>(direct);
    expect(SUCCEEDED(direct->Wait(fence, 3)) && SUCCEEDED(direct->Signal(other, 1)), "wait before signal recorded");
    expect(directQueue->Pending() == 2 && other->GetCompletedValue() == 0, "the wait and what follows are held back until a signal is promised");
    expect(SUCCEEDED(compute->Signal(fence, 3)), "the other queue signals");
    expect(reaches(other, 1) && directQueue->Pending() == 0, "the held work runs after the other queue's signal, without a deadlock");
    expect(SUCCEEDED(direct->Wait(fence, 5)) && SUCCEEDED(direct->Signal(other, 2)) && directQueue->Pending() == 2, "a wait for a CPU signal is held");
    expect(SUCCEEDED(fence->Signal(5)) && fence->GetCompletedValue() == 5, "CPU signal");
    expect(reaches(other, 2), "a CPU signal releases the held GPU wait");
    expect(SUCCEEDED(fence->Signal(5)), "signalling the current value again");
    expect(SUCCEEDED(fence->SetEventOnCompletion(4, nullptr)), "blocking wait on a reached value");
    CloseHandle(event);
    std::printf("fences and queues: %zu checks\n", checks);
    direct->Release();
    compute->Release();
    fence->Release();
    other->Release();
    if (validate) {
        std::printf("validation errors=%zu\n", device->ValidationErrors());
        for (const std::string& message : device->ValidationMessages()) std::printf("  %.600s\n", message.c_str());
        if (device->ValidationErrors() != 0) status = 1;
    }
    if (status == 0) std::puts("PASS fence gpu");
    return status;
}

int ArenaSelfTest() {
    BufferArena arena(0x10000000ull + 0x1000, 1u << 20);
    const std::optional<std::uint64_t> a = arena.Allocate(0x10000, 0x10000);
    const std::optional<std::uint64_t> b = arena.Allocate(0x20000, 0x10000);
    const std::optional<std::uint64_t> c = arena.Allocate(0x100, 0x100);
    if (!a || !b || !c || (0x10001000ull + *a) % 0x10000 != 0 || (0x10001000ull + *b) % 0x10000 != 0 || *b < *a + 0x10000 || arena.Used() != 0x30100) return Fail("arena allocations");
    if (arena.Allocate(0, 16) || arena.Allocate(16, 3) || arena.Allocate(2u << 20, 16)) return Fail("arena refusals");
    if (!arena.Free(*b) || arena.Free(*b) || arena.Used() != 0x10100) return Fail("arena free");
    const std::optional<std::uint64_t> again = arena.Allocate(0x20000, 0x10000);
    if (!again || *again != *b) return Fail("arena reuses the freed range");
    if (!arena.Free(*a) || !arena.Free(*again) || !arena.Free(*c) || arena.Used() != 0 || arena.FreeRanges() != 1) return Fail("arena coalesces back to one range");
    const std::optional<std::uint64_t> whole = arena.Allocate(1u << 20, 16);
    if (!whole || *whole != 0) return Fail("arena whole range after coalescing");
    std::puts("PASS arena selftest");
    return 0;
}

int ResourcesGpu(bool validate) {
    DeviceRequirements requirements;
    requirements.features = {"bufferDeviceAddress"};
    requirements.hostMemoryImport = true;
    requirements.validation = validate;
    std::string error;
    const std::unique_ptr<VulkanDevice> device = VulkanDevice::Create(requirements, error);
    if (!device) {
        std::printf("SKIP no usable Vulkan device: %s\n", error.c_str());
        return validate && error == "the Khronos validation layer is not installed" ? kNoValidation : kNoDevice;
    }
    const std::unique_ptr<TitleBufferWindow> window = TitleBufferWindow::Create(*device, 64u << 20, error);
    if (!window) return Fail("window: " + error);
    BufferArena arena(window->Start(), window->Size());
    const ResourceContext context{*device, *window, arena};
    int status = 0;
    std::size_t checks = 0;
    const auto expect = [&](bool ok, const std::string& what) {
        ++checks;
        if (!ok && status == 0) status = Fail(what);
    };
    const auto bufferDesc = [](UINT64 bytes) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc = {1, 0};
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return desc;
    };
    const auto heap = [](D3D12_HEAP_TYPE type) { return D3D12_HEAP_PROPERTIES{type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0}; };
    const auto create = [&](D3D12_HEAP_TYPE type, const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state, D3D12_HEAP_FLAGS flags = D3D12_HEAP_FLAG_NONE) {
        ID3D12Resource* resource = nullptr;
        const HRESULT result = Resource::CreateCommitted(context, nullptr, heap(type), flags, desc, state, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource), error);
        return SUCCEEDED(result) ? resource : nullptr;
    };
    ID3D12Resource* upload = create(D3D12_HEAP_TYPE_UPLOAD, bufferDesc(1u << 20), D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* readback = create(D3D12_HEAP_TYPE_READBACK, bufferDesc(1u << 20), D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* local = create(D3D12_HEAP_TYPE_DEFAULT, bufferDesc(100000), D3D12_RESOURCE_STATE_COMMON);
    expect(upload && readback && local, "create buffers: " + error);
    if (status != 0) return status;
    const D3D12_GPU_VIRTUAL_ADDRESS uploadAddress = upload->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS localAddress = local->GetGPUVirtualAddress();
    expect(uploadAddress % kBufferPlacementAlignment == 0 && localAddress % kBufferPlacementAlignment == 0 && window->Contains(uploadAddress, 1u << 20) &&
               window->Contains(localAddress, 100000) && uploadAddress != localAddress && uploadAddress < 0x100000000ull, "buffer addresses in the window, aligned and distinct");
    const D3D12_RESOURCE_DESC localDesc = local->GetDesc();
    expect(localDesc.Width == 100000 && localDesc.Alignment == kBufferPlacementAlignment && localDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER, "buffer description");
    D3D12_HEAP_PROPERTIES properties{};
    D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_DENY_BUFFERS;
    expect(SUCCEEDED(upload->GetHeapProperties(&properties, &heapFlags)) && properties.Type == D3D12_HEAP_TYPE_UPLOAD && heapFlags == D3D12_HEAP_FLAG_NONE, "heap properties");
    std::uint8_t* written = nullptr;
    expect(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&written))) && written != nullptr, "map upload buffer");
    for (std::uint32_t index = 0; index < 4096; ++index) written[index] = static_cast<std::uint8_t>(index * 7 + 1);
    upload->Unmap(0, nullptr);
    void* refused = nullptr;
    expect(upload->Map(1, nullptr, &refused) == E_INVALIDARG, "map of subresource 1 refused");

    D3D12_RESOURCE_DESC colorDesc = TextureDesc2D(DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    ID3D12Resource* color = create(D3D12_HEAP_TYPE_DEFAULT, colorDesc, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12Resource* depth = create(D3D12_HEAP_TYPE_DEFAULT, TextureDesc2D(DXGI_FORMAT_D32_FLOAT, 32, 32, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL), D3D12_RESOURCE_STATE_COPY_SOURCE);
    expect(color && depth, "create textures: " + error);
    if (status != 0) return status;
    expect(color->GetGPUVirtualAddress() == 0 && color->Map(0, nullptr, &refused) == E_INVALIDARG, "textures have no address and cannot be mapped");
    expect(color->GetDesc().Width == 64 && color->GetDesc().Format == DXGI_FORMAT_R8G8B8A8_UNORM, "texture description");

    Resource* uploadResource = dynamic_cast<Resource*>(upload);
    Resource* readbackResource = dynamic_cast<Resource*>(readback);
    Resource* colorResource = dynamic_cast<Resource*>(color);
    Resource* depthResource = dynamic_cast<Resource*>(depth);
    std::uint8_t* readBytes = nullptr;
    readback->Map(0, nullptr, reinterpret_cast<void**>(&readBytes));
    std::fill(readBytes, readBytes + (1u << 20), std::uint8_t{0xAB});
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT colorPrint{};
    colorPrint.Offset = 0x10000;
    colorPrint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depthPrint{};
    depthPrint.Offset = 0x20000;
    depthPrint.Footprint = {DXGI_FORMAT_D32_FLOAT, 32, 32, 1, 256};
    bool recorded = true;
    const bool submitted = device->Submit(
        [&](VkCommandBuffer commands) {
            CommandRecorder recorder(*device, commands);
            recorded = recorded && recorder.CopyBufferRegion(readbackResource->Buffer(), 0, uploadResource->Buffer(), 0, 4096, error);
            recorded = recorded && recorder.CopyTextureToBuffer(readbackResource->Buffer(), colorPrint, colorResource->Texture(), D3D12_RESOURCE_STATE_COPY_SOURCE, 0, nullptr, error);
            recorded = recorded && recorder.CopyTextureToBuffer(readbackResource->Buffer(), depthPrint, depthResource->Texture(), D3D12_RESOURCE_STATE_COPY_SOURCE, 0, nullptr, error);
            VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            host.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &host;
            device->Vk().cmdPipelineBarrier2(commands, &dependency);
        },
        error);
    expect(submitted && recorded, "copies between resources: " + error);
    if (status == 0) {
        bool same = true;
        for (std::uint32_t index = 0; index < 4096 && same; ++index) same = readBytes[index] == static_cast<std::uint8_t>(index * 7 + 1);
        expect(same, "a GPU copy between two buffer resources in the window");
        bool zeroColor = true;
        for (UINT row = 0; row < 64 && zeroColor; ++row) {
            for (UINT byte = 0; byte < 64 * 4 && zeroColor; ++byte) zeroColor = readBytes[colorPrint.Offset + row * 256 + byte] == 0;
        }
        expect(zeroColor, "a new color texture reads back as zero");
        float depthValue = 1.0f;
        std::memcpy(&depthValue, readBytes + depthPrint.Offset + 5 * 256 + 5 * 4, sizeof(depthValue));
        expect(depthValue == 0.0f, "a new depth texture reads back as zero");
    }
    readback->Unmap(0, nullptr);

    const std::uint64_t usedBefore = arena.Used();
    std::uint8_t* dirty = nullptr;
    local->Map(0, nullptr, reinterpret_cast<void**>(&dirty));
    std::fill(dirty, dirty + 100000, std::uint8_t{0x5C});
    local->Unmap(0, nullptr);
    local->Release();
    local = create(D3D12_HEAP_TYPE_DEFAULT, bufferDesc(100000), D3D12_RESOURCE_STATE_COMMON);
    std::uint8_t* fresh = nullptr;
    expect(local && local->GetGPUVirtualAddress() == localAddress && SUCCEEDED(local->Map(0, nullptr, reinterpret_cast<void**>(&fresh))) && fresh[0] == 0 && fresh[99999] == 0 &&
               arena.Used() == usedBefore, "a released buffer's range is reused and zeroed");

    D3D12_RESOURCE_DESC oddBuffer = bufferDesc(256);
    oddBuffer.Height = 2;
    expect(!create(D3D12_HEAP_TYPE_DEFAULT, oddBuffer, D3D12_RESOURCE_STATE_COMMON) && error.find("height, depth and mips of 1") != std::string::npos, "a buffer with height 2 refused");
    expect(!create(D3D12_HEAP_TYPE_UPLOAD, colorDesc, D3D12_RESOURCE_STATE_GENERIC_READ) && error.find("default heap") != std::string::npos, "a texture in an upload heap refused");
    expect(!create(D3D12_HEAP_TYPE_DEFAULT, bufferDesc(256), D3D12_RESOURCE_STATE_COMMON, D3D12_HEAP_FLAG_SHARED) && error.find("heap flags") != std::string::npos, "a shared heap refused");
    expect(!create(D3D12_HEAP_TYPE_DEFAULT, bufferDesc(128u << 20), D3D12_RESOURCE_STATE_COMMON) && error.find("no free range") != std::string::npos, "a buffer larger than the window refused");
    for (ID3D12Resource* resource : {upload, readback, local, color, depth}) {
        if (resource != nullptr) resource->Release();
    }
    expect(arena.Used() == 0, "every buffer range returned to the window");
    std::printf("resources: %zu checks\n", checks);
    if (validate) {
        std::printf("validation errors=%zu\n", device->ValidationErrors());
        for (const std::string& message : device->ValidationMessages()) std::printf("  %.600s\n", message.c_str());
        if (device->ValidationErrors() != 0) status = 1;
    }
    if (status == 0) std::puts("PASS resources gpu");
    return status;
}

}

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "resolve") == 0) return Resolve(argc, argv);
    if (argc >= 4 && std::strcmp(argv[1], "compute") == 0) return Compute(argc, argv);
    if (argc == 2 && std::strcmp(argv[1], "graphics-selftest") == 0) return GraphicsSelfTest();
    if (argc == 2 && std::strcmp(argv[1], "input-selftest") == 0) return InputSelfTest();
    if (argc >= 4 && std::strcmp(argv[1], "graphics-title") == 0) return GraphicsTitle(argc, argv);
    if (argc == 3 && std::strcmp(argv[1], "dump-test-shaders") == 0) return DumpTestShaders(Utf8Path(argv[2]));
    if (argc == 2 && std::strcmp(argv[1], "states-selftest") == 0) return StatesSelfTest();
    if (argc == 2 && std::strcmp(argv[1], "textures-selftest") == 0) return TexturesSelfTest();
    if (argc == 2 && std::strcmp(argv[1], "views-selftest") == 0) return ViewsSelfTest();
    if (argc == 2 && std::strcmp(argv[1], "commands-selftest") == 0) return CommandsSelfTest();
    if (argc == 2 && std::strcmp(argv[1], "draw-selftest") == 0) return DrawSelfTest();
    if (argc == 2 && std::strcmp(argv[1], "indirect-selftest") == 0) return IndirectSelfTest();
    if (argc == 2 && std::strcmp(argv[1], "buffers-selftest") == 0) return BuffersSelfTest();
    if (argc == 2 && std::strcmp(argv[1], "rootsig-selftest") == 0) return RootSignatureSelfTest();
    if (argc >= 2 && std::strcmp(argv[1], "fence-gpu") == 0) return FenceGpu(HasFlag(argc, argv, "--validate"));
    if (argc == 2 && std::strcmp(argv[1], "arena-selftest") == 0) return ArenaSelfTest();
    if (argc >= 2 && std::strcmp(argv[1], "resources-gpu") == 0) return ResourcesGpu(HasFlag(argc, argv, "--validate"));
    if (argc >= 2 && std::strcmp(argv[1], "planes-gpu") == 0) return PlanesGpu(HasFlag(argc, argv, "--validate"));
    if (argc >= 2 && std::strcmp(argv[1], "indirect-gpu") == 0) return IndirectGpu(HasFlag(argc, argv, "--validate"));
    if (argc >= 2 && std::strcmp(argv[1], "draw-gpu") == 0) return DrawGpu(HasFlag(argc, argv, "--validate"));
    if (argc >= 2 && std::strcmp(argv[1], "commands-gpu") == 0) return CommandsGpu(HasFlag(argc, argv, "--validate"));
    if (argc >= 2 && std::strcmp(argv[1], "views-gpu") == 0) return ViewsGpu(HasFlag(argc, argv, "--validate"));
    if (argc >= 2 && std::strcmp(argv[1], "textures-gpu") == 0) return TexturesGpu(HasFlag(argc, argv, "--validate"));
    if (argc >= 2 && std::strcmp(argv[1], "states-gpu") == 0) return StatesGpu(HasFlag(argc, argv, "--validate"));
    if (argc >= 5 && std::strcmp(argv[1], "graphics") == 0) return GraphicsRequests(argc, argv);
    if (argc >= 2 && std::strcmp(argv[1], "graphics-gpu") == 0) return GraphicsGpu(HasFlag(argc, argv, "--validate"));
    std::puts("usage: resolve <store> <container|none> [--flags N] [--node N] [--cached N] | compute <store> <containers file> [--cache F] [--limit BYTES] [--threads N] [--features a,b] [--maintenance8] [--save] | graphics-selftest | graphics-gpu [--validate] | input-selftest | graphics-title <store> <vertex shader container> [--features a,b] [--maintenance8] | dump-test-shaders <folder> | graphics <store> <vertex container> <pixel container> [--layout] [--cache F] [--limit BYTES] [--save] | states-selftest | states-gpu [--validate] | textures-selftest | textures-gpu [--validate] | views-selftest | views-gpu [--validate] | commands-selftest | commands-gpu [--validate] | draw-selftest | draw-gpu [--validate] | indirect-selftest | indirect-gpu [--validate] | planes-gpu [--validate] | buffers-selftest | rootsig-selftest | fence-gpu [--validate] | arena-selftest | resources-gpu [--validate]");
    return 2;
}
