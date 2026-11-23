#include "xdll/d3d12_x/d3d12_x.hpp"

#include <cstdint>
#include <cstring>

ABX_API HRESULT D3D12ConfigureVirtualMemoryX(const void* description) {
    if (description == nullptr) {
        AbxUnsupported(__func__, "a null virtual memory description");
    }
    std::uint32_t words[8];
    std::memcpy(words, description, sizeof(words));
    AbxLog("d3d12_x", "D3D12ConfigureVirtualMemoryX(%08x %08x %08x %08x %08x %08x %08x %08x): GPU virtual memory is laid out by AnyBox, the description is recorded only",
           words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7]);
    return S_OK;
}
