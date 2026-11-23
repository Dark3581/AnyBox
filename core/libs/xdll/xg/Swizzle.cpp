#include "xdll/xg/xg.hpp"

#include <atomic>

namespace {

constexpr std::uint32_t kConsoleLayoutBase = 0x100;
constexpr std::uint32_t kLinearSwizzle = 0;

}

ABX_API HRESULT XGSuggestSwizzleMode(const void* desc, std::uint32_t flags, std::uint32_t* layout) {
    if (desc == nullptr || layout == nullptr) {
        return E_INVALIDARG;
    }
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("xg", "XGSuggestSwizzleMode suggests the linear layout 0x%x (flags 0x%x): AnyBox chooses the layout of textures the title leaves to the platform",
               kConsoleLayoutBase | kLinearSwizzle, flags);
    }
    *layout = kConsoleLayoutBase | kLinearSwizzle;
    return S_OK;
}
