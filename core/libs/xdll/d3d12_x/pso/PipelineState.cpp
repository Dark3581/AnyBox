#include "PipelineState.hpp"

#include "AbxApi.hpp"

#include <cstdio>
#include <filesystem>

namespace abx::d3d12x {

namespace {

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

}
