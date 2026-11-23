#include "xdll/xg/xg.hpp"

#include "CopyLayout.hpp"
#include "TextureDesc.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

namespace {

constexpr std::size_t kSlots = 64;
constexpr UINT kConsoleLayoutBase = 0x100;

struct TextureComputer {
    const void* const* table;
    std::atomic<ULONG> references{1};
    D3D12_RESOURCE_DESC desc{};
    std::uint32_t mips = 1;
    abx::d3d12x::Footprints footprints;
};

template <std::size_t Slot>
void Unmapped(TextureComputer*) {
    AbxUnsupported("xg texture computer", ("slot " + std::to_string(Slot) + " of the texture computer is not mapped: no measured call site shows which method it is").c_str());
}

template <std::size_t... Slots>
std::array<const void*, kSlots> UnmappedTable(std::index_sequence<Slots...>) {
    return {reinterpret_cast<const void*>(&Unmapped<Slots>)...};
}

ULONG ComputerRelease(TextureComputer* self) {
    const ULONG left = --self->references;
    if (left == 0) delete self;
    return left;
}

HRESULT ComputerCopyIntoSubresource(TextureComputer* self, void* base, UINT plane, UINT subresource, const void* source, UINT rowPitch, UINT slicePitch) {
    if (base == nullptr || source == nullptr || plane != 0) {
        AbxUnsupported("xg CopyIntoSubresource", "a null destination or source, or a plane other than 0");
    }
    if (subresource >= self->footprints.layouts.size()) {
        AbxUnsupported("xg CopyIntoSubresource", "a subresource past the end of the texture");
    }
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = self->footprints.layouts[subresource];
    const UINT rows = self->footprints.rows[subresource];
    const UINT64 rowBytes = self->footprints.rowBytes[subresource];
    if (rowPitch < rowBytes) {
        AbxUnsupported("xg CopyIntoSubresource", "a source row pitch shorter than a row of the subresource");
    }
    auto* destination = static_cast<std::uint8_t*>(base) + footprint.Offset;
    const auto* from = static_cast<const std::uint8_t*>(source);
    for (UINT slice = 0; slice < footprint.Footprint.Depth; ++slice) {
        for (UINT row = 0; row < rows; ++row) {
            std::memcpy(destination + (static_cast<std::size_t>(slice) * rows + row) * footprint.Footprint.RowPitch,
                        from + static_cast<std::size_t>(slice) * slicePitch + static_cast<std::size_t>(row) * rowPitch, static_cast<std::size_t>(rowBytes));
        }
    }
    return S_OK;
}

std::array<const void*, kSlots> ComputerTable() {
    std::array<const void*, kSlots> table = UnmappedTable(std::make_index_sequence<kSlots>{});
    table[1] = reinterpret_cast<const void*>(&ComputerRelease);
    table[8] = reinterpret_cast<const void*>(&ComputerCopyIntoSubresource);
    return table;
}

const std::array<const void*, kSlots> kComputerTable = ComputerTable();

}

ABX_API HRESULT XGCreateTextureComputer(const D3D12_RESOURCE_DESC* desc, void** computer) {
    if (desc == nullptr || computer == nullptr) {
        return E_INVALIDARG;
    }
    *computer = nullptr;
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER || desc->Dimension == D3D12_RESOURCE_DIMENSION_UNKNOWN) {
        AbxUnsupported(__func__, "a texture computer for a buffer");
    }
    const UINT layout = static_cast<UINT>(desc->Layout);
    if (layout < kConsoleLayoutBase || layout > kConsoleLayoutBase + 0xFF) {
        AbxUnsupported(__func__, ("texture layout " + std::to_string(layout) + " is not a console swizzle layout").c_str());
    }
    auto created = new TextureComputer();
    created->table = kComputerTable.data();
    created->desc = *desc;
    created->desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    created->desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<UINT>(desc->Flags) & 0xFFFFu);
    created->mips = desc->MipLevels != 0 ? desc->MipLevels : abx::d3d12x::FullMipChain(desc->Width, desc->Height, desc->DepthOrArraySize);
    const UINT layers = desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc->DepthOrArraySize;
    std::string error;
    std::optional<abx::d3d12x::Footprints> footprints = abx::d3d12x::DesktopCopyableFootprints(created->desc, created->mips, 0, created->mips * layers, 0, error);
    if (!footprints) {
        delete created;
        AbxUnsupported(__func__, error.c_str());
    }
    created->footprints = std::move(*footprints);
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("xg", "texture computers lay textures out linearly with D3D12 copyable footprints (first requested swizzle layout 0x%x); d3d12_x reads the same layout", layout);
    }
    *computer = created;
    return S_OK;
}
