#pragma once

#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <optional>
#include <string>

namespace abx::d3d12x {

enum class IndirectKind { Draw, DrawIndexed, Dispatch, DispatchMesh };

struct IndirectSignature {
    IndirectKind kind = IndirectKind::Draw;
    UINT stride = 0;
};

std::uint32_t IndirectArgumentBytes(IndirectKind kind);
std::optional<IndirectSignature> TranslateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC& desc, std::string& error);

}
