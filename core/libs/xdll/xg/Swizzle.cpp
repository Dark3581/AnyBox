#include "xdll/xg/xg.hpp"

#include "Gfx10Tiling.hpp"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

namespace {

namespace gfx10 = abx::d3d12x::gfx10;

bool FirstReport(const std::string& key) {
    static std::mutex lock;
    static std::set<std::string> reported;
    const std::lock_guard guard(lock);
    return reported.insert(key).second;
}

}

ABX_API HRESULT XGSuggestSwizzleMode(const void* desc, std::uint32_t flags, std::uint32_t* layout) {
    if (desc == nullptr || layout == nullptr) {
        if (FirstReport("null")) AbxLog("xg", "XGSuggestSwizzleMode returns 0x%08lx: a null resource description or output", static_cast<unsigned long>(E_POINTER));
        return E_POINTER;
    }
    D3D12_RESOURCE_DESC copy{};
    std::memcpy(&copy, desc, sizeof(copy));
    gfx10::Refusal fallback;
    const std::uint32_t suggested = gfx10::SuggestSwizzleLayout(copy, gfx10::ConsolePipes(AbxSetting("runtime.console_model")), fallback);
    if (fallback.code != S_OK && FirstReport(fallback.reason)) {
        AbxLog("xg", "XGSuggestSwizzleMode suggests 0x%x by use alone (argument 0x%x, dimension %u, %llux%u, format %u, flags 0x%x): the layout model refuses the texture with 0x%08lx: %s",
               suggested, flags, static_cast<unsigned>(copy.Dimension), static_cast<unsigned long long>(copy.Width), copy.Height, static_cast<unsigned>(copy.Format),
               static_cast<unsigned>(copy.Flags), static_cast<unsigned long>(fallback.code), fallback.reason.c_str());
    }
    if (FirstReport("first")) {
        AbxLog("xg", "XGSuggestSwizzleMode answers from the GFX10 layout model (first: 0x%x for dimension %u, %llux%u, format %u, flags 0x%x)", suggested,
               static_cast<unsigned>(copy.Dimension), static_cast<unsigned long long>(copy.Width), copy.Height, static_cast<unsigned>(copy.Format), static_cast<unsigned>(copy.Flags));
    }
    *layout = suggested;
    return S_OK;
}
