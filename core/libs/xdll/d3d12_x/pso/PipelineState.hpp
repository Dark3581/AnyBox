#pragma once

#include "PipelineLibrary.hpp"
#include "ShaderStore.hpp"

#include <windows.h>
#include <d3d12.h>

#include <optional>
#include <string>

namespace abx::d3d12x {

std::optional<PipelineSource> ComputePipelineSource(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, const ShaderStore& store, std::string& error);
std::optional<GraphicsRequest> GraphicsPipelineRequest(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, const ShaderStore& store, std::string& error);

}
