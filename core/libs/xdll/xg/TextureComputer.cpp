#include "xdll/xg/xg.hpp"

#include "Gfx10Tiling.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <algorithm>
#include <vector>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>

namespace {

namespace gfx10 = abx::d3d12x::gfx10;

constexpr std::size_t kSlots = 64;
constexpr std::size_t kMappedSlots = 12;

struct TextureComputer {
    const void* const* table;
    std::atomic<ULONG> references{1};
    gfx10::ConsoleLayout layout;
};

bool FirstReport(const std::string& key) {
    static std::mutex lock;
    static std::set<std::string> reported;
    const std::lock_guard guard(lock);
    return reported.insert(key).second;
}

void Refuse(const char* call, HRESULT code, const std::string& reason, const std::string& detail = {}) {
    if (!FirstReport(std::string(call) + "|" + std::to_string(static_cast<unsigned long>(code)) + "|" + reason)) return;
    AbxLog("xg", "%s returns 0x%08lx: %s%s%s", call, static_cast<unsigned long>(code), reason.c_str(), detail.empty() ? "" : " ", detail.c_str());
}

void AnswerZero(const char* call, const std::string& reason) {
    if (!FirstReport(std::string(call) + "|zero|" + reason)) return;
    AbxLog("xg", "%s returns 0: %s", call, reason.c_str());
}

template <std::size_t Slot>
void Unmapped(TextureComputer*) {
    AbxUnsupported("xg texture computer", ("slot " + std::to_string(Slot) + " of the texture computer is not mapped: the console's computer has 12 methods").c_str());
}

template <std::size_t Slot>
HRESULT NotImplemented(TextureComputer*) {
    Refuse("XGTextureAddressComputer", E_NOTIMPL, "slot " + std::to_string(Slot) + " has no model");
    return E_NOTIMPL;
}

template <std::size_t... Slots>
std::array<const void*, kSlots> UnmappedTable(std::index_sequence<Slots...>) {
    return {reinterpret_cast<const void*>(&Unmapped<Slots>)...};
}

ULONG ComputerAddRef(TextureComputer* self) {
    return ++self->references;
}

ULONG ComputerRelease(TextureComputer* self) {
    const ULONG left = --self->references;
    if (left == 0) delete self;
    return left;
}

HRESULT ComputerGetResourceLayout(TextureComputer* self, std::uint8_t* layout) {
    if (layout == nullptr) {
        Refuse("GetResourceLayout", E_POINTER, "a null XG_RESOURCE_LAYOUT");
        return E_POINTER;
    }
    self->layout.Write(std::span<std::uint8_t, gfx10::kResourceLayoutBytes>(layout, gfx10::kResourceLayoutBytes));
    return S_OK;
}

UINT64 ComputerGetResourceSizeBytes(TextureComputer* self) {
    return self->layout.Size();
}

UINT64 ComputerGetResourceBaseAlignmentBytes(TextureComputer* self) {
    return self->layout.Alignment();
}

UINT64 ComputerGetMipLevelOffsetBytes(TextureComputer* self, UINT plane, UINT mip) {
    const std::optional<gfx10::PlaneView> view = self->layout.PlaneSurface(plane);
    if (!view || mip >= view->surface->chain.levels.size()) {
        AnswerZero("GetMipLevelOffsetBytes", "a plane without texels or a mip past the chain");
        return 0;
    }
    return view->offset + view->surface->MipOffset(mip);
}

UINT64 ComputerGetTexelElementOffsetBytes(TextureComputer* self, UINT plane, UINT mip, UINT64 x, UINT y, UINT slice, UINT, UINT* bit) {
    if (bit != nullptr) *bit = 0;
    const std::optional<gfx10::PlaneView> view = self->layout.PlaneSurface(plane);
    if (!view || mip >= view->surface->chain.levels.size() || slice >= view->surface->Slices(mip).first) {
        AnswerZero("GetTexelElementOffsetBytes", "a plane without texels, a mip past the chain or a slice past the level");
        return 0;
    }
    return view->offset + view->surface->ElementOffset(mip, static_cast<std::size_t>(x), y, slice);
}

using HostView = const void* (*)(std::uint64_t);

HostView TitleHostView() {
    static const HostView view = [] {
        const HMODULE d3d12 = GetModuleHandleW(L"ab_d3d12_x.dll");
        return d3d12 != nullptr ? reinterpret_cast<HostView>(reinterpret_cast<void*>(GetProcAddress(d3d12, "AbxD3D12TitleHostView"))) : nullptr;
    }();
    return view;
}

bool Accessible(const void* start, std::uint64_t bytes, bool write) {
    std::uint64_t at = reinterpret_cast<std::uint64_t>(start);
    const std::uint64_t end = at + bytes;
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const DWORD readable = writable | PAGE_READONLY | PAGE_EXECUTE_READ;
    while (at < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT ||
            (region.Protect & (write ? writable : readable)) == 0 || (region.Protect & PAGE_GUARD) != 0) {
            return false;
        }
        at = reinterpret_cast<std::uint64_t>(region.BaseAddress) + region.RegionSize;
    }
    return true;
}

bool ThroughHostViews(std::uint64_t start, std::uint64_t bytes, std::uint8_t* buffer, bool write) {
    constexpr std::uint64_t kPage = 0x10000;
    const HostView view = TitleHostView();
    if (view == nullptr) return false;
    std::uint64_t done = 0;
    while (done < bytes) {
        const std::uint64_t at = start + done;
        const std::uint64_t piece = (std::min)(kPage - (at & (kPage - 1)), bytes - done);
        auto* host = const_cast<std::uint8_t*>(static_cast<const std::uint8_t*>(view(at)));
        if (host == nullptr) {
            if (!Accessible(reinterpret_cast<const void*>(at), piece, write)) return false;
            host = reinterpret_cast<std::uint8_t*>(at);
        }
        if (write) std::memcpy(host, buffer + done, static_cast<std::size_t>(piece));
        else std::memcpy(buffer + done, host, static_cast<std::size_t>(piece));
        done += piece;
    }
    return true;
}

void RequireAccess(const void* start, std::uint64_t bytes, bool write, const char* function) {
    std::uint64_t at = reinterpret_cast<std::uint64_t>(start);
    const std::uint64_t end = at + bytes;
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const DWORD readable = writable | PAGE_READONLY | PAGE_EXECUTE_READ;
    while (at < end) {
        MEMORY_BASIC_INFORMATION region{};
        const bool queried = VirtualQuery(reinterpret_cast<const void*>(at), &region, sizeof(region)) == sizeof(region);
        if (!queried || region.State != MEM_COMMIT || (region.Protect & (write ? writable : readable)) == 0 || (region.Protect & PAGE_GUARD) != 0) {
            char text[240];
            std::snprintf(text, sizeof(text), "the texture memory at 0x%llx (in 0x%llx + %llu bytes) is not %s CPU memory: state 0x%lx, protection 0x%lx, type 0x%lx",
                          static_cast<unsigned long long>(at), static_cast<unsigned long long>(reinterpret_cast<std::uint64_t>(start)), static_cast<unsigned long long>(bytes),
                          write ? "writable" : "readable", queried ? region.State : 0ul, queried ? region.Protect : 0ul, queried ? region.Type : 0ul);
            AbxUnsupported(function, text);
        }
        at = reinterpret_cast<std::uint64_t>(region.BaseAddress) + region.RegionSize;
    }
}

HRESULT ComputerCopyIntoSubresource(TextureComputer* self, void* resource, UINT plane, UINT subresource, const void* source, UINT rowPitch, UINT slicePitch);
HRESULT ComputerCopyFromSubresource(TextureComputer* self, void* destination, UINT plane, UINT subresource, const void* resource, UINT rowPitch, UINT slicePitch);

void RequireZeroExtra(UINT extra, const char* function) {
    if (extra != 0) {
        AbxUnsupported(function, ("the fifth argument is " + std::to_string(extra) + "; the title's call site (game+0x979f57) passes 0 there, other values are not measured").c_str());
    }
}

HRESULT ConsoleCopyIntoSubresource(TextureComputer* self, void* resource, UINT plane, UINT subresource, UINT extra, const void* source, UINT rowPitch, UINT slicePitch) {
    RequireZeroExtra(extra, "xg CopyIntoSubresource");
    return ComputerCopyIntoSubresource(self, resource, plane, subresource, source, rowPitch, slicePitch);
}

HRESULT ConsoleCopyFromSubresource(TextureComputer* self, void* destination, UINT plane, UINT subresource, UINT extra, const void* resource, UINT rowPitch, UINT slicePitch) {
    RequireZeroExtra(extra, "xg CopyFromSubresource");
    return ComputerCopyFromSubresource(self, destination, plane, subresource, resource, rowPitch, slicePitch);
}

HRESULT ComputerCopyIntoSubresource(TextureComputer* self, void* resource, UINT plane, UINT subresource, const void* source, UINT rowPitch, UINT slicePitch) {
    if (resource != nullptr && !Accessible(resource, self->layout.Size(), true)) {
        std::vector<std::uint8_t> copy(static_cast<std::size_t>(self->layout.Size()));
        const auto start = reinterpret_cast<std::uint64_t>(resource);
        if (ThroughHostViews(start, copy.size(), copy.data(), false)) {
            std::string error;
            const HRESULT result = gfx10::CopyIntoSubresource(self->layout, copy.data(), plane, subresource, static_cast<const std::uint8_t*>(source), rowPitch, slicePitch, error);
            if (SUCCEEDED(result) && ThroughHostViews(start, copy.size(), copy.data(), true)) return result;
        }
        RequireAccess(resource, self->layout.Size(), true, "xg CopyIntoSubresource");
    }
    std::string error;
    const HRESULT result = gfx10::CopyIntoSubresource(self->layout, static_cast<std::uint8_t*>(resource), plane, subresource, static_cast<const std::uint8_t*>(source), rowPitch,
                                                      slicePitch, error);
    if (FAILED(result)) Refuse("CopyIntoSubresource", result, error);
    return result;
}

HRESULT ComputerCopyFromSubresource(TextureComputer* self, void* destination, UINT plane, UINT subresource, const void* resource, UINT rowPitch, UINT slicePitch);

HRESULT CopyFromIntoHostViews(TextureComputer* self, void* destination, UINT plane, UINT subresource, const void* resource, UINT rowPitch, UINT slicePitch) {
    const std::optional<gfx10::PlaneView> view = self->layout.PlaneSurface(plane);
    if (!view || view->surface == nullptr || view->surface->chain.levels.empty()) return E_INVALIDARG;
    const auto& levels = view->surface->chain.levels;
    const gfx10::MipLevel& level = levels[subresource % levels.size()];
    const std::uint64_t depth = level.depth > 1 ? level.depth : 1;
    const std::uint64_t extent = (depth - 1) * slicePitch + (level.height > 0 ? (level.height - 1) * static_cast<std::uint64_t>(rowPitch) : 0) +
                                 level.width * view->surface->bytesPerElement;
    if (extent > (512ull << 20)) {
        char text[240];
        std::snprintf(text, sizeof(text), "a copy destination extent of %llu bytes (subresource %u, %zux%zu elements, depth %llu, row pitch %u, slice pitch %u, %u bytes per element)",
                      static_cast<unsigned long long>(extent), subresource, level.width, level.height, static_cast<unsigned long long>(depth), rowPitch, slicePitch,
                      view->surface->bytesPerElement);
        AbxUnsupported("xg CopyFromSubresource (destination)", text);
    }
    std::vector<std::uint8_t> copy(static_cast<std::size_t>(extent));
    const auto start = reinterpret_cast<std::uint64_t>(destination);
    if (!ThroughHostViews(start, copy.size(), copy.data(), false)) return E_FAIL;
    const HRESULT result = ComputerCopyFromSubresource(self, copy.data(), plane, subresource, resource, rowPitch, slicePitch);
    if (SUCCEEDED(result) && !ThroughHostViews(start, copy.size(), copy.data(), true)) return E_FAIL;
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("xg", "texture copies to or from title memory the CPU has not mapped go through AnyBox's views of the physical pages the title mapped there (first destination 0x%llx)",
               static_cast<unsigned long long>(start));
    }
    return result;
}

HRESULT ComputerCopyFromSubresource(TextureComputer* self, void* destination, UINT plane, UINT subresource, const void* resource, UINT rowPitch, UINT slicePitch) {
    if (destination != nullptr && rowPitch != 0 && !Accessible(destination, rowPitch, true)) {
        const HRESULT viaViews = CopyFromIntoHostViews(self, destination, plane, subresource, resource, rowPitch, slicePitch);
        if (viaViews != E_FAIL) return viaViews;
        RequireAccess(destination, rowPitch, true, "xg CopyFromSubresource (destination)");
    }
    if (resource != nullptr && !Accessible(resource, self->layout.Size(), false)) {
        if (self->layout.Size() > (1ull << 30)) AbxUnsupported("xg CopyFromSubresource", ("a texture layout of " + std::to_string(self->layout.Size()) + " bytes").c_str());
        std::vector<std::uint8_t> copy(static_cast<std::size_t>(self->layout.Size()));
        if (ThroughHostViews(reinterpret_cast<std::uint64_t>(resource), copy.size(), copy.data(), false)) {
            std::string error;
            return gfx10::CopyFromSubresource(self->layout, static_cast<std::uint8_t*>(destination), plane, subresource, copy.data(), rowPitch, slicePitch, error);
        }
        RequireAccess(resource, self->layout.Size(), false, "xg CopyFromSubresource");
    }
    std::string error;
    const HRESULT result = gfx10::CopyFromSubresource(self->layout, static_cast<std::uint8_t*>(destination), plane, subresource, static_cast<const std::uint8_t*>(resource),
                                                      rowPitch, slicePitch, error);
    if (FAILED(result)) Refuse("CopyFromSubresource", result, error);
    return result;
}

HRESULT ComputerGetResourceTiling(TextureComputer* self, UINT* totalTiles, D3D12_PACKED_MIP_INFO* packed, D3D12_TILE_SHAPE* shape, UINT* tilingCount, UINT first,
                                  D3D12_SUBRESOURCE_TILING* tilings) {
    std::string error;
    const HRESULT result = gfx10::ResourceTiling(self->layout, totalTiles, packed, shape, tilingCount, first, tilings, error);
    if (FAILED(result)) Refuse("GetResourceTiling", result, error);
    return result;
}

std::array<const void*, kSlots> ComputerTable() {
    std::array<const void*, kSlots> table = UnmappedTable(std::make_index_sequence<kSlots>{});
    const std::array<const void*, kMappedSlots> mapped{
        reinterpret_cast<const void*>(&ComputerAddRef),
        reinterpret_cast<const void*>(&ComputerRelease),
        reinterpret_cast<const void*>(&ComputerGetResourceLayout),
        reinterpret_cast<const void*>(&ComputerGetResourceSizeBytes),
        reinterpret_cast<const void*>(&ComputerGetResourceBaseAlignmentBytes),
        reinterpret_cast<const void*>(&ComputerGetMipLevelOffsetBytes),
        reinterpret_cast<const void*>(&ComputerGetTexelElementOffsetBytes),
        reinterpret_cast<const void*>(&NotImplemented<7>),
        reinterpret_cast<const void*>(&ComputerCopyIntoSubresource),
        reinterpret_cast<const void*>(&ConsoleCopyFromSubresource),
        reinterpret_cast<const void*>(&ComputerGetResourceTiling),
        reinterpret_cast<const void*>(&NotImplemented<11>),
    };
    for (std::size_t slot = 0; slot < kMappedSlots; ++slot) table[slot] = mapped[slot];
    return table;
}

const std::array<const void*, kSlots> kComputerTable = ComputerTable();

std::string DescribeDesc(const D3D12_RESOURCE_DESC& desc) {
    char text[192];
    std::snprintf(text, sizeof(text), "(dimension %u, %llux%u, %u slices, %u mips, format %u, %u samples, layout 0x%x, flags 0x%x)", static_cast<unsigned>(desc.Dimension),
                  static_cast<unsigned long long>(desc.Width), desc.Height, static_cast<unsigned>(desc.DepthOrArraySize), static_cast<unsigned>(desc.MipLevels),
                  static_cast<unsigned>(desc.Format), desc.SampleDesc.Count, static_cast<unsigned>(desc.Layout), static_cast<unsigned>(desc.Flags));
    return text;
}

}

ABX_API HRESULT XGCreateTextureComputer(const D3D12_RESOURCE_DESC* desc, void** computer) {
    if (desc == nullptr || computer == nullptr) {
        Refuse(__func__, E_POINTER, "a null resource description or output");
        return E_POINTER;
    }
    *computer = nullptr;
    gfx10::Refusal refusal;
    std::optional<gfx10::ConsoleLayout> layout = gfx10::DescribeConsoleLayout(*desc, gfx10::ConsolePipes(AbxSetting("runtime.console_model")), refusal);
    if (!layout) {
        Refuse(__func__, refusal.code, refusal.reason, DescribeDesc(*desc));
        return refusal.code;
    }
    auto created = new TextureComputer();
    created->table = kComputerTable.data();
    created->layout = std::move(*layout);
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("xg", "texture computers answer from the GFX10 layout model with the Series X pipe configuration (first: %s, %llu bytes, alignment 0x%llx)", DescribeDesc(*desc).c_str(),
               static_cast<unsigned long long>(created->layout.Size()), static_cast<unsigned long long>(created->layout.Alignment()));
    }
    *computer = created;
    return S_OK;
}
