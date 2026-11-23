#include "xdll/xgameruntime/xgameruntime.hpp"

ABX_API bool XPackageIsPackagedProcess() {
    return false;
}

ABX_API HRESULT XPackageEnumeratePackages(std::uint32_t kind, std::uint32_t scope, void* context, void* callback) {
    static_cast<void>(context);
    if (callback == nullptr) {
        return E_INVALIDARG;
    }
    AbxTrace("XPackageEnumeratePackages(kind %u, scope %u): no packages are installed beside the title", kind, scope);
    return S_OK;
}

ABX_API HRESULT XPackageGetCurrentProcessPackageIdentifier(std::size_t bufferSize, char* buffer) {
    static_cast<void>(bufferSize);
    static_cast<void>(buffer);
    return E_GAMEPACKAGE_APP_NOT_PACKAGED;
}
