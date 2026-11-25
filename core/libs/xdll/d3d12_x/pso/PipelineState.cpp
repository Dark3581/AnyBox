#include "PipelineState.hpp"

#include "AbxApi.hpp"
#include "GraphicsState.hpp"
#include "VertexInput.hpp"

#include <cstdio>
#include <filesystem>

namespace abx::d3d12x {

namespace {

const ShaderModule* FindStage(const D3D12_SHADER_BYTECODE& bytecode, const ShaderStore& store, const char* stage, const char* name, std::string& error) {
    const ShaderModule* module = store.Find(bytecode.pShaderBytecode, bytecode.BytecodeLength);
    if (module == nullptr) {
        const std::optional<std::size_t> size = ContainerSize(bytecode.pShaderBytecode, bytecode.BytecodeLength);
        const std::optional<Sha256> digest = size ? Sha256Of(bytecode.pShaderBytecode, *size) : std::nullopt;
        error = size ? std::string("the ") + name + " " + (digest ? Hex(*digest) : std::string("with an unhashable container")) + " is not in the shader store"
                     : std::string("the ") + name + " is not a DXBC container";
        return nullptr;
    }
    if (module->stage != stage) {
        error = "the shader store has stage " + module->stage + " for the " + name + " " + module->key;
        return nullptr;
    }
    return module;
}

std::string HexNumber(unsigned value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%x", value);
    return text;
}

}

std::optional<PipelineSource> ComputePipelineSource(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, const ShaderStore& store, std::string& error) {
    if (desc.CS.pShaderBytecode == nullptr || desc.CS.BytecodeLength == 0) {
        error = "the description has no compute shader";
        return std::nullopt;
    }
    if (desc.Flags != D3D12_PIPELINE_STATE_FLAG_NONE) {
        error = "pipeline state flags " + HexNumber(static_cast<unsigned>(desc.Flags)) + " are not supported";
        return std::nullopt;
    }
    if (desc.NodeMask > 1) {
        error = "node mask " + HexNumber(desc.NodeMask) + " names a GPU node other than the first";
        return std::nullopt;
    }
    if (desc.CachedPSO.pCachedBlob != nullptr && desc.CachedPSO.CachedBlobSizeInBytes != 0) {
        AbxTrace("compute pipeline state: ignoring a cached blob of %zu bytes", static_cast<std::size_t>(desc.CachedPSO.CachedBlobSizeInBytes));
    }
    const ShaderModule* module = store.Find(desc.CS.pShaderBytecode, desc.CS.BytecodeLength);
    if (module == nullptr) {
        const std::optional<std::size_t> size = ContainerSize(desc.CS.pShaderBytecode, desc.CS.BytecodeLength);
        if (!size) {
            error = "the compute shader is not a DXBC container";
            return std::nullopt;
        }
        const std::optional<Sha256> digest = Sha256Of(desc.CS.pShaderBytecode, *size);
        error = "the compute shader " + (digest ? Hex(*digest) : std::string("with an unhashable container")) + " is not in the shader store";
        return std::nullopt;
    }
    if (module->stage != "cs") {
        error = "the shader store has stage " + module->stage + " for compute shader " + module->key;
        return std::nullopt;
    }
    return PipelineSource{module->key, std::filesystem::path(module->path), module->subgroupSize};
}

std::optional<GraphicsRequest> GraphicsPipelineRequest(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, const ShaderStore& store, std::string& error) {
    if (desc.VS.pShaderBytecode == nullptr || desc.VS.BytecodeLength == 0) {
        error = "the description has no vertex shader";
        return std::nullopt;
    }
    if (desc.CachedPSO.pCachedBlob != nullptr && desc.CachedPSO.CachedBlobSizeInBytes != 0) {
        AbxTrace("graphics pipeline state: ignoring a cached blob of %zu bytes", static_cast<std::size_t>(desc.CachedPSO.CachedBlobSizeInBytes));
    }
    std::optional<GraphicsPipelineState> state = TranslateGraphicsState(desc, error);
    if (!state) {
        return std::nullopt;
    }
    GraphicsRequest request;
    const ShaderModule* vertex = FindStage(desc.VS, store, "vs", "vertex shader", error);
    if (vertex == nullptr) {
        return std::nullopt;
    }
    request.shaders.push_back({VK_SHADER_STAGE_VERTEX_BIT, vertex->key, std::filesystem::path(vertex->path), vertex->subgroupSize});
    if (desc.GS.pShaderBytecode != nullptr && desc.GS.BytecodeLength != 0) {
        const ShaderModule* geometry = FindStage(desc.GS, store, "gs", "geometry shader", error);
        if (geometry == nullptr) {
            return std::nullopt;
        }
        request.shaders.push_back({VK_SHADER_STAGE_GEOMETRY_BIT, geometry->key, std::filesystem::path(geometry->path), geometry->subgroupSize});
        state->features.insert("geometryShader");
    }
    if (desc.PS.pShaderBytecode != nullptr && desc.PS.BytecodeLength != 0) {
        const ShaderModule* pixel = FindStage(desc.PS, store, "ps", "pixel shader", error);
        if (pixel == nullptr) {
            return std::nullopt;
        }
        request.shaders.push_back({VK_SHADER_STAGE_FRAGMENT_BIT, pixel->key, std::filesystem::path(pixel->path), pixel->subgroupSize});
    }
    std::optional<VertexInput> input = TranslateInputLayout(desc.InputLayout, vertex->inputs, error);
    if (!input) {
        return std::nullopt;
    }
    request.state = std::move(*state);
    request.input = std::move(*input);
    for (const GraphicsShaderSource& shader : request.shaders) {
        request.key += shader.key + ":";
    }
    request.key += GraphicsStateKey(request.state, request.input);
    return request;
}

std::optional<GraphicsRequest> MeshPipelineRequest(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, const D3D12_SHADER_BYTECODE& mesh, const D3D12_SHADER_BYTECODE& amplification,
                                                   const ShaderStore& store, std::string& error) {
    if (mesh.pShaderBytecode == nullptr || mesh.BytecodeLength == 0) {
        error = "the description has no mesh shader";
        return std::nullopt;
    }
    if (amplification.pShaderBytecode != nullptr && amplification.BytecodeLength != 0) {
        error = "amplification shaders are not in the shader store";
        return std::nullopt;
    }
    if (desc.VS.pShaderBytecode != nullptr || desc.GS.pShaderBytecode != nullptr || desc.HS.pShaderBytecode != nullptr || desc.DS.pShaderBytecode != nullptr) {
        error = "a mesh pipeline with vertex, geometry or tessellation shaders";
        return std::nullopt;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC states = desc;
    states.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    states.InputLayout = {};
    std::optional<GraphicsPipelineState> state = TranslateGraphicsState(states, error);
    if (!state) {
        return std::nullopt;
    }
    state->features.insert("meshShader");
    GraphicsRequest request;
    const ShaderModule* meshModule = FindStage(mesh, store, "ms", "mesh shader", error);
    if (meshModule == nullptr) {
        return std::nullopt;
    }
    request.shaders.push_back({VK_SHADER_STAGE_MESH_BIT_EXT, meshModule->key, std::filesystem::path(meshModule->path), meshModule->subgroupSize});
    if (desc.PS.pShaderBytecode != nullptr && desc.PS.BytecodeLength != 0) {
        const ShaderModule* pixel = FindStage(desc.PS, store, "ps", "pixel shader", error);
        if (pixel == nullptr) {
            return std::nullopt;
        }
        request.shaders.push_back({VK_SHADER_STAGE_FRAGMENT_BIT, pixel->key, std::filesystem::path(pixel->path), pixel->subgroupSize});
    }
    request.state = std::move(*state);
    request.key = "mesh:";
    for (const GraphicsShaderSource& shader : request.shaders) {
        request.key += shader.key + ":";
    }
    request.key += GraphicsStateKey(request.state, request.input);
    return request;
}

}
