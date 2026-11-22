#include "GraphicsPipeline.hpp"
#include "GraphicsState.hpp"
#include "PipelineCompiler.hpp"
#include "PipelineLibrary.hpp"
#include "PipelineState.hpp"
#include "ShaderStore.hpp"
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
        const unsigned threads = static_cast<unsigned>(std::max(1ul, std::strtoul(FindOption(argc, argv, "--threads", "1").c_str(), nullptr, 10)));
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

}

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "resolve") == 0) return Resolve(argc, argv);
    if (argc >= 4 && std::strcmp(argv[1], "compute") == 0) return Compute(argc, argv);
    if (argc == 2 && std::strcmp(argv[1], "graphics-selftest") == 0) return GraphicsSelfTest();
    if (argc >= 2 && std::strcmp(argv[1], "graphics-gpu") == 0) return GraphicsGpu(HasFlag(argc, argv, "--validate"));
    std::puts("usage: resolve <store> <container|none> [--flags N] [--node N] [--cached N] | compute <store> <containers file> [--cache F] [--limit BYTES] [--threads N] [--features a,b] [--maintenance8] [--save] | graphics-selftest | graphics-gpu [--validate]");
    return 2;
}
