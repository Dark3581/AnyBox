#include "xdll/xgameruntime/xgameruntime.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

bool Same(const GUID* a, const GUID& b) {
    return a != nullptr && std::memcmp(a, &b, sizeof(GUID)) == 0;
}

constexpr GUID kUnknownInterface{0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

struct ApiObject {
    const void* const* table;
    const GUID* id;
    const char* name;
};

HRESULT STDMETHODCALLTYPE ObjectQueryInterface(ApiObject* self, REFIID riid, void** object) {
    if (object == nullptr) return E_POINTER;
    if (std::memcmp(&riid, self->id, sizeof(GUID)) == 0 || std::memcmp(&riid, &kUnknownInterface, sizeof(GUID)) == 0) {
        *object = self;
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE ObjectAddRef(ApiObject*) { return 1; }
ULONG STDMETHODCALLTYPE ObjectRelease(ApiObject*) { return 1; }

template <int Slot>
HRESULT Unmapped(ApiObject* self) {
    const std::string message = std::string("slot ") + std::to_string(Slot) + " of the GDK API interface " + GuidText(self->id) + " (" + self->name +
                                ") is not mapped: no measured call site shows which function it is";
    AbxUnsupported("QueryApiImpl", message.c_str());
}

HRESULT ThreadingAsyncGetStatus(ApiObject*, XAsyncBlock* block, bool wait) { return XAsyncGetStatus(block, wait); }
void ThreadingAsyncCancel(ApiObject*, XAsyncBlock* block) { XAsyncCancel(block); }
HRESULT ThreadingAsyncBegin(ApiObject*, XAsyncBlock* block, void* context, const void* identity, const char* identityName, XAsyncProvider* provider) {
    return XAsyncBegin(block, context, identity, identityName, provider);
}
HRESULT ThreadingAsyncSchedule(ApiObject*, XAsyncBlock* block, std::uint32_t delay) { return XAsyncSchedule(block, delay); }
void ThreadingAsyncComplete(ApiObject*, XAsyncBlock* block, HRESULT result, std::size_t required) { XAsyncComplete(block, result, required); }
HRESULT ThreadingAsyncGetResult(ApiObject*, XAsyncBlock* block, const void* identity, std::size_t size, void* buffer, std::size_t* used) {
    return XAsyncGetResult(block, identity, size, buffer, used);
}
HRESULT ThreadingQueueCreate(ApiObject*, XTaskQueueDispatchMode work, XTaskQueueDispatchMode completion, XTaskQueueHandle* queue) {
    return XTaskQueueCreate(work, completion, queue);
}
HRESULT ThreadingQueueCreateComposite(ApiObject*, XTaskQueuePortHandle work, XTaskQueuePortHandle completion, XTaskQueueHandle* queue) {
    return XTaskQueueCreateComposite(work, completion, queue);
}
HRESULT ThreadingQueueGetPort(ApiObject*, XTaskQueueHandle queue, XTaskQueuePort port, XTaskQueuePortHandle* handle) { return XTaskQueueGetPort(queue, port, handle); }
HRESULT ThreadingQueueDuplicateHandle(ApiObject*, XTaskQueueHandle queue, XTaskQueueHandle* duplicated) { return XTaskQueueDuplicateHandle(queue, duplicated); }
bool ThreadingQueueDispatch(ApiObject*, XTaskQueueHandle queue, XTaskQueuePort port, std::uint32_t timeout) { return XTaskQueueDispatch(queue, port, timeout); }
void ThreadingQueueCloseHandle(ApiObject*, XTaskQueueHandle queue) { XTaskQueueCloseHandle(queue); }
HRESULT ThreadingQueueSubmitCallback(ApiObject*, XTaskQueueHandle queue, XTaskQueuePort port, void* context, XTaskQueueCallback* callback) {
    return XTaskQueueSubmitCallback(queue, port, context, callback);
}
HRESULT ThreadingQueueSubmitDelayedCallback(ApiObject*, XTaskQueueHandle queue, XTaskQueuePort port, std::uint32_t delay, void* context, XTaskQueueCallback* callback) {
    return XTaskQueueSubmitDelayedCallback(queue, port, delay, context, callback);
}
void ThreadingAssertNotTimeSensitive(ApiObject*) { XThreadAssertNotTimeSensitive(); }
HRESULT ThreadingQueueTerminate(ApiObject*, XTaskQueueHandle queue, bool wait, void* context, XTaskQueueTerminatedCallback* callback) {
    return XTaskQueueTerminate(queue, wait, context, callback);
}

template <typename Function>
const void* Entry(Function function) {
    return reinterpret_cast<const void*>(function);
}

constexpr GUID kThreadingInterface{0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};

const void* const kThreadingTable[] = {
    Entry(&ObjectQueryInterface),
    Entry(&ObjectAddRef),
    Entry(&ObjectRelease),
    Entry(&ThreadingAsyncGetStatus),
    Entry(&Unmapped<4>),
    Entry(&ThreadingAsyncCancel),
    Entry(&Unmapped<6>),
    Entry(&ThreadingAsyncBegin),
    Entry(&Unmapped<8>),
    Entry(&ThreadingAsyncSchedule),
    Entry(&ThreadingAsyncComplete),
    Entry(&ThreadingAsyncGetResult),
    Entry(&ThreadingQueueCreate),
    Entry(&ThreadingQueueCreateComposite),
    Entry(&ThreadingQueueGetPort),
    Entry(&ThreadingQueueDuplicateHandle),
    Entry(&ThreadingQueueDispatch),
    Entry(&ThreadingQueueCloseHandle),
    Entry(&ThreadingQueueSubmitCallback),
    Entry(&ThreadingQueueSubmitDelayedCallback),
    Entry(&Unmapped<20>),
    Entry(&Unmapped<21>),
    Entry(&ThreadingQueueTerminate),
    Entry(&Unmapped<23>),
    Entry(&Unmapped<24>),
    Entry(&Unmapped<25>),
    Entry(&Unmapped<26>),
    Entry(&Unmapped<27>),
    Entry(&Unmapped<28>),
    Entry(&ThreadingAssertNotTimeSensitive),
    Entry(&Unmapped<30>),
};

ApiObject g_threading{kThreadingTable, &kThreadingInterface, "threading: XAsync and XTaskQueue"};

struct ApiInterface {
    const GUID& provider;
    const GUID& interfaceId;
    ApiObject& object;
};

const ApiInterface kInterfaces[] = {
    {kThreadingInterface, kThreadingInterface, g_threading},
};

}

ABX_API HRESULT QueryApiImpl(const GUID* providerId, const GUID* interfaceId, void** implementation) {
    if (implementation == nullptr) {
        return E_POINTER;
    }
    *implementation = nullptr;
    for (const ApiInterface& entry : kInterfaces) {
        if (Same(providerId, entry.provider) && Same(interfaceId, entry.interfaceId)) {
            *implementation = &entry.object;
            return S_OK;
        }
    }
    const std::string message = "the GDK API interface " + GuidText(interfaceId) + " of provider " + GuidText(providerId) +
                                " is not implemented: its method table layout has to be mapped before AnyBox can serve it";
    AbxUnsupported(__func__, message.c_str());
}

ABX_API HRESULT InitializeApiImpl(std::uint32_t first, std::uint32_t second) {
    AbxLog("xgameruntime", "InitializeApiImpl(%s, %s)", Hex(first).c_str(), Hex(second).c_str());
    return S_OK;
}

ABX_API HRESULT InitializeApiImplEx(std::uint64_t first, std::uint64_t second, std::uint64_t flags) {
    AbxLog("xgameruntime", "InitializeApiImplEx(%s, %s, flags %s)", Hex(first).c_str(), Hex(second).c_str(), Hex(flags).c_str());
    return S_OK;
}

ABX_API HRESULT InitializeApiImplEx2(std::uint64_t first, std::uint64_t second, std::uint64_t flags, void* extra) {
    if (extra != nullptr) {
        const std::string message = "the GDK runtime handshake (" + Hex(first) + ", " + Hex(second) + ", flags " + Hex(flags) +
                                    ") passed a fourth argument, whose layout and use are unknown";
        AbxUnsupported(__func__, message.c_str());
    }
    AbxLog("xgameruntime", "InitializeApiImplEx2(%s, %s, flags %s)", Hex(first).c_str(), Hex(second).c_str(), Hex(flags).c_str());
    return S_OK;
}

ABX_API HRESULT UninitializeApiImpl() { AbxNotImplemented(__func__); }

ABX_API void XErrorReport(HRESULT result, const char* message) {
    AbxLog("xgameruntime", "XErrorReport 0x%08lx: %s", static_cast<unsigned long>(result), message != nullptr ? message : "(no message)");
}
