#pragma once

#include "ConsoleFacade.hpp"

#include <windows.h>
#include <d3d12.h>

namespace abx::d3d12x::console {

enum class Kind { CommandQueue, CommandAllocator, CommandList, Fence, DescriptorHeap, Resource, RootSignature, PipelineState, QueryHeap, CommandSignature, TitleMemoryRange };

constexpr GUID kCommandListInterface{0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};

HRESULT Deliver(Kind kind, IUnknown* inner, REFIID riid, void** object);

template <typename Object>
Object* Unwrap(void* object) {
    return static_cast<Object*>(FacadeInner(object));
}

}
