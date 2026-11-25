#include "IndirectCommands.hpp"

namespace abx::d3d12x {

std::uint32_t IndirectArgumentBytes(IndirectKind kind) {
    switch (kind) {
    case IndirectKind::Draw:
        return sizeof(D3D12_DRAW_ARGUMENTS);
    case IndirectKind::DrawIndexed:
        return sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    case IndirectKind::Dispatch:
        return sizeof(D3D12_DISPATCH_ARGUMENTS);
    case IndirectKind::DispatchMesh:
        return sizeof(D3D12_DISPATCH_MESH_ARGUMENTS);
    }
    return 0;
}

std::optional<IndirectSignature> TranslateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC& desc, std::string& error) {
    if (desc.NodeMask > 1) {
        error = "node mask " + std::to_string(desc.NodeMask) + " names a GPU node other than the first";
        return std::nullopt;
    }
    if (desc.NumArgumentDescs == 0 || desc.pArgumentDescs == nullptr) {
        error = "the command signature has no arguments";
        return std::nullopt;
    }
    for (UINT index = 0; index + 1 < desc.NumArgumentDescs; ++index) {
        const int type = static_cast<int>(desc.pArgumentDescs[index].Type);
        if (type != D3D12_INDIRECT_ARGUMENT_TYPE_DRAW && type != D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED && type != D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH &&
            type != D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH) {
            error = "indirect argument type " + std::to_string(type) + " (constants, views or buffer bindings changed per command) is not supported";
        } else {
            error = "the draw or dispatch argument is not the last argument of the command signature";
        }
        return std::nullopt;
    }
    IndirectSignature signature;
    constexpr int kConsoleDispatchX = 105;
    if (static_cast<int>(desc.pArgumentDescs[desc.NumArgumentDescs - 1].Type) == kConsoleDispatchX) {
        signature.kind = IndirectKind::Dispatch;
        if (desc.ByteStride < IndirectArgumentBytes(signature.kind) || desc.ByteStride % 4 != 0) {
            error = "byte stride " + std::to_string(desc.ByteStride) + " is too small for the console DispatchX (105) arguments or not a multiple of 4";
            return std::nullopt;
        }
        signature.stride = desc.ByteStride;
        return signature;
    }
    switch (desc.pArgumentDescs[desc.NumArgumentDescs - 1].Type) {
    case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
        signature.kind = IndirectKind::Draw;
        break;
    case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
        signature.kind = IndirectKind::DrawIndexed;
        break;
    case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
        signature.kind = IndirectKind::Dispatch;
        break;
    case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH:
        signature.kind = IndirectKind::DispatchMesh;
        break;
    default:
        error = "indirect argument type " + std::to_string(static_cast<int>(desc.pArgumentDescs[desc.NumArgumentDescs - 1].Type)) +
                " as the last argument is not a supported draw or dispatch";
        return std::nullopt;
    }
    const std::uint32_t bytes = IndirectArgumentBytes(signature.kind);
    if (desc.ByteStride < bytes || desc.ByteStride % 4 != 0) {
        error = "byte stride " + std::to_string(desc.ByteStride) + " is smaller than the " + std::to_string(bytes) + "-byte arguments or not a multiple of 4";
        return std::nullopt;
    }
    signature.stride = desc.ByteStride;
    return signature;
}

}
