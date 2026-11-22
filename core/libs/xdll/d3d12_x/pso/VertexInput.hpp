#pragma once

#include "GraphicsPipeline.hpp"
#include "ShaderStore.hpp"

#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace abx::d3d12x {

std::optional<std::uint32_t> VertexFormatBytes(DXGI_FORMAT format);
InputType VertexFormatType(DXGI_FORMAT format);

std::optional<VertexInput> TranslateInputLayout(const D3D12_INPUT_LAYOUT_DESC& layout, const std::vector<ShaderInput>& inputs, std::string& error);

}
