#include "ConsoleObjects.hpp"

#include "AbxApi.hpp"
#include "CommandList.hpp"
#include "DescriptorHeap.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace abx::d3d12x::console {
namespace {

HRESULT STDMETHODCALLTYPE ListClose(Facade* self) {
    return InnerOf<CommandList>(self)->Close();
}

template <std::size_t Slot>
void STDMETHODCALLTYPE ListRecordedCommand(Facade*, std::uint32_t first, std::uint32_t second, std::uint32_t third, std::uint64_t fourth) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "console command list slot %zu (0x%x, 0x%x, 0x%x, 0x%llx): recorded and not applied, later calls are traced only", Slot, first, second, third,
               static_cast<unsigned long long>(fourth));
    } else {
        AbxTrace("console command list slot %zu (0x%x, 0x%x, 0x%x, 0x%llx): not applied", Slot, first, second, third, static_cast<unsigned long long>(fourth));
    }
}

SIZE_T HeapCpuStart(Facade* self) {
    return InnerOf<DescriptorHeap>(self)->CpuStart().ptr;
}

UINT64 HeapGpuStart(Facade* self) {
    return InnerOf<DescriptorHeap>(self)->GpuStart().ptr;
}

template <typename Function>
void Map(FacadeType& type, std::size_t slot, Function function) {
    type.table[slot] = reinterpret_cast<const void*>(function);
}

const FacadeType& TypeOf(Kind kind) {
    static const FacadeType queue = MakeFacadeType("command queue", {__uuidof(ID3D12CommandQueue)});
    static const FacadeType allocator = MakeFacadeType("command allocator", {__uuidof(ID3D12CommandAllocator)});
    static const FacadeType list = [] {
        FacadeType type = MakeFacadeType("command list", {kCommandListInterface, __uuidof(ID3D12GraphicsCommandList)});
        Map(type, 9, &ListClose);
        Map(type, 131, &ListRecordedCommand<131>);
        Map(type, 132, &ListRecordedCommand<132>);
        return type;
    }();
    static const FacadeType fence = MakeFacadeType("fence", {__uuidof(ID3D12Fence)});
    static const FacadeType heap = [] {
        FacadeType type = MakeFacadeType("descriptor heap", {__uuidof(ID3D12DescriptorHeap)});
        Map(type, 9, &HeapCpuStart);
        Map(type, 10, &HeapGpuStart);
        return type;
    }();
    static const FacadeType resource = MakeFacadeType("resource", {__uuidof(ID3D12Resource)});
    switch (kind) {
    case Kind::CommandQueue: return queue;
    case Kind::CommandAllocator: return allocator;
    case Kind::CommandList: return list;
    case Kind::Fence: return fence;
    case Kind::DescriptorHeap: return heap;
    case Kind::Resource: return resource;
    }
    return resource;
}

}

HRESULT Deliver(Kind kind, IUnknown* inner, REFIID riid, void** object) {
    if (object == nullptr) {
        inner->Release();
        return E_POINTER;
    }
    *object = nullptr;
    const FacadeType& type = TypeOf(kind);
    bool served = false;
    for (const GUID& id : type.interfaces) served = served || std::memcmp(&riid, &id, sizeof(GUID)) == 0;
    if (!served) {
        inner->Release();
        AbxUnsupported(("D3D12 " + type.name).c_str(), ("the interface " + GuidText(riid) + " is not one the console " + type.name + " facade serves").c_str());
    }
    *object = WrapInFacade(type, inner);
    return S_OK;
}

}
