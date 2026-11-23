#pragma once

#include <windows.h>
#include <d3d12.h>

#include <cstddef>
#include <optional>
#include <string>

namespace abx::d3d12x {

struct ParsedPipelineStream {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
    D3D12_SHADER_BYTECODE compute{};
    D3D12_SHADER_BYTECODE mesh{};
    D3D12_SHADER_BYTECODE amplification{};
    std::string types;
};

std::optional<ParsedPipelineStream> ParsePipelineStream(const void* stream, std::size_t bytes, std::string& error);

}
