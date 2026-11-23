#include "xdll/xgameruntime/xgameruntime.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

std::string GuidText(const GUID* guid) {
    if (guid == nullptr) {
        return "null";
    }
    char text[40];
    std::snprintf(text, sizeof(text), "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", static_cast<unsigned long>(guid->Data1), guid->Data2, guid->Data3, guid->Data4[0],
                  guid->Data4[1], guid->Data4[2], guid->Data4[3], guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]);
    return text;
}

std::string Hex(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

}

ABX_API HRESULT QueryApiImpl(const GUID* providerId, const GUID* interfaceId, void** implementation) {
    if (implementation != nullptr) *implementation = nullptr;
    const std::string message = "the GDK API interface " + GuidText(interfaceId) + " of provider " + GuidText(providerId) +
                                " is not implemented: its method table layout has to be mapped before AnyBox can serve it";
    AbxUnsupported(__func__, message.c_str());
}

ABX_API HRESULT InitializeApiImpl(std::uint32_t first, std::uint32_t second) {
    const std::string message = "the GDK runtime handshake (" + Hex(first) + ", " + Hex(second) + ") is not implemented: what the runtime checks or records is unknown";
    AbxUnsupported(__func__, message.c_str());
}

ABX_API HRESULT InitializeApiImplEx(std::uint64_t first, std::uint64_t second, std::uint64_t flags) {
    const std::string message =
        "the GDK runtime handshake (" + Hex(first) + ", " + Hex(second) + ", flags " + Hex(flags) + ") is not implemented: what the runtime checks or records is unknown";
    AbxUnsupported(__func__, message.c_str());
}

ABX_API HRESULT InitializeApiImplEx2(std::uint64_t first, std::uint64_t second, std::uint64_t flags, void* extra) {
    const std::string message = "the GDK runtime handshake (" + Hex(first) + ", " + Hex(second) + ", flags " + Hex(flags) + ", " +
                                Hex(reinterpret_cast<std::uintptr_t>(extra)) + ") is not implemented: what the runtime checks or records is unknown";
    AbxUnsupported(__func__, message.c_str());
}

ABX_API HRESULT UninitializeApiImpl() { AbxNotImplemented(__func__); }

ABX_API void XErrorReport(HRESULT result, const char* message) {
    AbxLog("xgameruntime", "XErrorReport 0x%08lx: %s", static_cast<unsigned long>(result), message != nullptr ? message : "(no message)");
}
