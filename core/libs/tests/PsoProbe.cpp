#include "GraphicsPipeline.hpp"
#include "GraphicsState.hpp"
#include "PipelineCompiler.hpp"
#include "PipelineLibrary.hpp"
#include "PipelineState.hpp"
#include "ResourceState.hpp"
#include "ShaderStore.hpp"
#include "TextureDesc.hpp"
#include "VertexInput.hpp"
#include "VulkanDevice.hpp"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
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
    if (argc >= 2 && std::strcmp(argv[1], "textures-gpu") == 0) return TexturesGpu(HasFlag(argc, argv, "--validate"));
    if (argc >= 2 && std::strcmp(argv[1], "states-gpu") == 0) return StatesGpu(HasFlag(argc, argv, "--validate"));
    if (argc >= 5 && std::strcmp(argv[1], "graphics") == 0) return GraphicsRequests(argc, argv);
    if (argc >= 2 && std::strcmp(argv[1], "graphics-gpu") == 0) return GraphicsGpu(HasFlag(argc, argv, "--validate"));
    std::puts("usage: resolve <store> <container|none> [--flags N] [--node N] [--cached N] | compute <store> <containers file> [--cache F] [--limit BYTES] [--threads N] [--features a,b] [--maintenance8] [--save] | graphics-selftest | graphics-gpu [--validate] | input-selftest | graphics-title <store> <vertex shader container> [--features a,b] [--maintenance8] | dump-test-shaders <folder> | graphics <store> <vertex container> <pixel container> [--layout] [--cache F] [--limit BYTES] [--save] | states-selftest | states-gpu [--validate] | textures-selftest | textures-gpu [--validate]");
    return 2;
}
