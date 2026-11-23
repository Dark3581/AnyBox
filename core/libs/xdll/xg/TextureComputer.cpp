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

constexpr std::size_t kLayoutPlanes = 0x18;
constexpr std::size_t kPlaneBytes = 0x6c8;
constexpr std::size_t kPlaneSliceStride = 0x10;
constexpr std::size_t kPlaneMips = 0x38;
constexpr std::size_t kMipBytes = 0x70;
constexpr std::size_t kMipEntries = 15;
constexpr std::size_t kMipSize = 0x00;
constexpr std::size_t kMipOffset = 0x08;
constexpr std::size_t kMipRows = 0x10;
constexpr std::size_t kMipRowPitch = 0x1c;

template <typename Value>
void Store(std::uint8_t* base, std::size_t offset, Value value) {
    std::memcpy(base + offset, &value, sizeof(value));
}

HRESULT ComputerGetResourceLayout(TextureComputer* self, std::uint8_t* layout) {
    if (layout == nullptr) return E_POINTER;
    const auto& footprints = self->footprints;
    if (self->mips > kMipEntries) {
        AbxUnsupported("xg GetResourceLayout", "more than 15 mip levels");
    }
    std::memset(layout, 0, kLayoutPlanes + kPlaneBytes);
    Store<std::uint64_t>(layout, 0x00, footprints.totalBytes);
    Store<std::uint64_t>(layout, 0x08, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
    Store<std::uint32_t>(layout, 0x10, 1u);
    Store<std::uint32_t>(layout, 0x14, self->mips);
    std::uint8_t* plane = layout + kLayoutPlanes;
    const std::uint64_t sliceStride = footprints.layouts.size() > self->mips ? footprints.layouts[self->mips].Offset : footprints.totalBytes;
    Store<std::uint64_t>(plane, 0x00, footprints.totalBytes);
    Store<std::uint64_t>(plane, kPlaneSliceStride, sliceStride);
    for (std::uint32_t mip = 0; mip < self->mips; ++mip) {
        std::uint8_t* entry = plane + kPlaneMips + mip * kMipBytes;
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = footprints.layouts[mip];
        const std::uint64_t rows = footprints.rows[mip];
        Store<std::uint64_t>(entry, kMipSize, rows * footprint.Footprint.RowPitch * footprint.Footprint.Depth);
        Store<std::uint64_t>(entry, kMipOffset, footprint.Offset);
        Store<std::uint64_t>(entry, kMipRows, rows);
        Store<std::uint32_t>(entry, kMipRowPitch, footprint.Footprint.RowPitch);
    }
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("xg", "texture computer layouts fill the measured fields (slice stride, per-mip size, offset, rows and row pitch); the rest of the record is zero");
    }
    return S_OK;
}

std::array<const void*, kSlots> ComputerTable() {
    std::array<const void*, kSlots> table = UnmappedTable(std::make_index_sequence<kSlots>{});
    table[1] = reinterpret_cast<const void*>(&ComputerRelease);
    table[2] = reinterpret_cast<const void*>(&ComputerGetResourceLayout);
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
