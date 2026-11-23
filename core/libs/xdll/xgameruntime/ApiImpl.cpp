#include "xdll/xgameruntime/xgameruntime.hpp"

#include <atomic>
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

constexpr GUID kNetworkingProvider{0x37e56907, 0x2f10, 0x41e8, {0xb7, 0x2f, 0x36, 0xed, 0xb1, 0x85, 0x33, 0x1a}};
constexpr GUID kNetworkingInterface{0xbf2346b2, 0x39af, 0x4658, {0xb5, 0xea, 0x44, 0x71, 0x3c, 0x7e, 0x83, 0xb3}};

constexpr std::size_t kConnectivityHintBytes = 16;
constexpr std::size_t kConnectivityLevelOffset = 0;
constexpr std::size_t kNetworkInitializedOffset = 12;

HRESULT NetworkingGetConnectivityHint(ApiObject*, std::uint8_t* hint) {
    if (hint == nullptr) {
        AbxUnsupported("QueryApiImpl", "the connectivity hint method was called without an output");
    }
    std::memset(hint, 0, kConnectivityHintBytes);
    const std::uint32_t noInternet = 0;
    std::memcpy(hint + kConnectivityLevelOffset, &noInternet, sizeof(noInternet));
    hint[kNetworkInitializedOffset] = 1;
    return S_OK;
}

const void* const kNetworkingTable[] = {
    Entry(&ObjectQueryInterface),
    Entry(&ObjectAddRef),
    Entry(&ObjectRelease),
    Entry(&Unmapped<3>),
    Entry(&Unmapped<4>),
    Entry(&Unmapped<5>),
    Entry(&Unmapped<6>),
    Entry(&Unmapped<7>),
    Entry(&Unmapped<8>),
    Entry(&Unmapped<9>),
    Entry(&Unmapped<10>),
    Entry(&Unmapped<11>),
    Entry(&Unmapped<12>),
    Entry(&Unmapped<13>),
    Entry(&Unmapped<14>),
    Entry(&NetworkingGetConnectivityHint),
    Entry(&Unmapped<16>),
    Entry(&Unmapped<17>),
};

ApiObject g_networking{kNetworkingTable, &kNetworkingInterface, "networking: connectivity"};

constexpr GUID kUserInterface{0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};

std::atomic<std::uint64_t> g_nextToken{1};

HRESULT RegisterWithoutEvents(void*, void*, void*, std::uint64_t* token) {
    if (token == nullptr) {
        AbxUnsupported("QueryApiImpl", "an event registration without a token output");
    }
    *token = g_nextToken++;
    return S_OK;
}

HRESULT UserRegisterForChangeEvent(ApiObject*, XTaskQueueHandle queue, void* context, void* callback, std::uint64_t* token) {
    return RegisterWithoutEvents(queue, context, callback, token);
}

const void* const kUserTable[] = {
    Entry(&ObjectQueryInterface),
    Entry(&ObjectAddRef),
    Entry(&ObjectRelease),
    Entry(&Unmapped<3>),
    Entry(&Unmapped<4>),
    Entry(&Unmapped<5>),
    Entry(&Unmapped<6>),
    Entry(&Unmapped<7>),
    Entry(&Unmapped<8>),
    Entry(&Unmapped<9>),
    Entry(&Unmapped<10>),
    Entry(&Unmapped<11>),
    Entry(&Unmapped<12>),
    Entry(&Unmapped<13>),
    Entry(&Unmapped<14>),
    Entry(&Unmapped<15>),
    Entry(&Unmapped<16>),
    Entry(&Unmapped<17>),
    Entry(&Unmapped<18>),
    Entry(&Unmapped<19>),
    Entry(&Unmapped<20>),
    Entry(&Unmapped<21>),
    Entry(&Unmapped<22>),
    Entry(&Unmapped<23>),
    Entry(&Unmapped<24>),
    Entry(&Unmapped<25>),
    Entry(&Unmapped<26>),
    Entry(&Unmapped<27>),
    Entry(&Unmapped<28>),
    Entry(&Unmapped<29>),
    Entry(&Unmapped<30>),
    Entry(&Unmapped<31>),
    Entry(&Unmapped<32>),
    Entry(&UserRegisterForChangeEvent),
    Entry(&Unmapped<34>),
};

ApiObject g_user{kUserTable, &kUserInterface, "user"};

constexpr GUID kPackageProvider{0xaf406016, 0xe850, 0x4aa8, {0xa8, 0x8d, 0x2f, 0x3d, 0xcb, 0x9d, 0xac, 0x7e}};
constexpr GUID kPackageInterface{0x0b9d556e, 0xab70, 0x43f4, {0xba, 0xa0, 0x99, 0x99, 0xac, 0x4c, 0xb0, 0x40}};

bool PackageIsPackagedProcess(ApiObject*) { return XPackageIsPackagedProcess(); }
HRESULT PackageEnumeratePackages(ApiObject*, std::uint32_t kind, std::uint32_t scope, void* context, void* callback) {
    return XPackageEnumeratePackages(kind, scope, context, callback);
}

const void* const kPackageTable[] = {
    Entry(&ObjectQueryInterface),
    Entry(&ObjectAddRef),
    Entry(&ObjectRelease),
    Entry(&Unmapped<3>),
    Entry(&PackageIsPackagedProcess),
    Entry(&Unmapped<5>),
    Entry(&Unmapped<6>),
    Entry(&Unmapped<7>),
    Entry(&Unmapped<8>),
    Entry(&Unmapped<9>),
    Entry(&Unmapped<10>),
    Entry(&Unmapped<11>),
    Entry(&Unmapped<12>),
    Entry(&Unmapped<13>),
    Entry(&Unmapped<14>),
    Entry(&Unmapped<15>),
    Entry(&Unmapped<16>),
    Entry(&Unmapped<17>),
    Entry(&Unmapped<18>),
    Entry(&Unmapped<19>),
    Entry(&Unmapped<20>),
    Entry(&Unmapped<21>),
    Entry(&Unmapped<22>),
    Entry(&Unmapped<23>),
    Entry(&Unmapped<24>),
    Entry(&Unmapped<25>),
    Entry(&Unmapped<26>),
    Entry(&Unmapped<27>),
    Entry(&Unmapped<28>),
    Entry(&Unmapped<29>),
    Entry(&Unmapped<30>),
    Entry(&Unmapped<31>),
    Entry(&Unmapped<32>),
    Entry(&Unmapped<33>),
    Entry(&Unmapped<34>),
    Entry(&Unmapped<35>),
    Entry(&Unmapped<36>),
    Entry(&Unmapped<37>),
    Entry(&Unmapped<38>),
    Entry(&PackageEnumeratePackages),
};

ApiObject g_package{kPackageTable, &kPackageInterface, "package"};

constexpr GUID kPersistentStorageProvider{0xf4faf4d4, 0x2d04, 0x4fce, {0xb3, 0xe0, 0x47, 0x4a, 0x71, 0x3a, 0x3e, 0x84}};
constexpr GUID kPersistentStorageInterface{0x41a4e10c, 0x5a7e, 0x41d9, {0x8c, 0x37, 0x37, 0xbd, 0xe6, 0x2a, 0x07, 0xd6}};

HRESULT PersistentStorageGetPath(ApiObject*, std::size_t size, char* path, std::size_t* used) {
    return XPersistentLocalStorageGetPath(size, path, used);
}

const void* const kPersistentStorageTable[] = {
    Entry(&ObjectQueryInterface),
    Entry(&ObjectAddRef),
    Entry(&ObjectRelease),
    Entry(&Unmapped<3>),
    Entry(&PersistentStorageGetPath),
    Entry(&Unmapped<5>),
};

ApiObject g_persistentStorage{kPersistentStorageTable, &kPersistentStorageInterface, "persistent local storage"};

constexpr GUID kDisplayProvider{0x03f0fe74, 0xfdd9, 0x4e5c, {0xb6, 0x30, 0xf9, 0x33, 0x9c, 0x47, 0xac, 0xc5}};
constexpr GUID kDisplayInterface{0x35f07670, 0x706e, 0x4bfb, {0x94, 0x76, 0x09, 0x07, 0x98, 0xc5, 0xeb, 0xf3}};

std::uint32_t DisplayTryEnableHdrMode(ApiObject*, std::uint32_t preference, void* info) {
    return XDisplayTryEnableHdrMode(preference, info);
}

const void* const kDisplayTable[] = {
    Entry(&ObjectQueryInterface),
    Entry(&ObjectAddRef),
    Entry(&ObjectRelease),
    Entry(&Unmapped<3>),
    Entry(&DisplayTryEnableHdrMode),
};

ApiObject g_display{kDisplayTable, &kDisplayInterface, "display"};

constexpr GUID kOptionalFeatureProvider{0x3e241977, 0x6237, 0x41e9, {0x85, 0x59, 0x10, 0x2c, 0x2d, 0x99, 0x83, 0xf1}};
constexpr GUID kOptionalFeatureInterface{0xd722b373, 0x8c4d, 0x4692, {0x9e, 0x51, 0xd6, 0xad, 0x9b, 0x37, 0xaa, 0x7d}};

HRESULT OptionalFeatureCreate(ApiObject*, void** created) {
    if (created != nullptr) *created = nullptr;
    AbxLog("xgameruntime", "provider {3e241977-6237-41e9-8559-102c2d9983f1} slot 9 (creates an object into its argument) answers E_NOTIMPL; its only caller treats any failure as the feature being absent");
    return E_NOTIMPL;
}

const void* const kOptionalFeatureTable[] = {
    Entry(&ObjectQueryInterface), Entry(&ObjectAddRef), Entry(&ObjectRelease), Entry(&Unmapped<3>),  Entry(&Unmapped<4>),  Entry(&Unmapped<5>),
    Entry(&Unmapped<6>),          Entry(&Unmapped<7>),  Entry(&Unmapped<8>),   Entry(&OptionalFeatureCreate), Entry(&Unmapped<10>), Entry(&Unmapped<11>),
    Entry(&Unmapped<12>),         Entry(&Unmapped<13>), Entry(&Unmapped<14>),  Entry(&Unmapped<15>), Entry(&Unmapped<16>), Entry(&Unmapped<17>),
    Entry(&Unmapped<18>),         Entry(&Unmapped<19>), Entry(&Unmapped<20>),  Entry(&Unmapped<21>),
};

ApiObject g_optionalFeature{kOptionalFeatureTable, &kOptionalFeatureInterface, "provider 3e241977"};

struct ApiInterface {
    const GUID& provider;
    const GUID& interfaceId;
    ApiObject& object;
};

const ApiInterface kInterfaces[] = {
    {kThreadingInterface, kThreadingInterface, g_threading},
    {kNetworkingProvider, kNetworkingInterface, g_networking},
    {kUserInterface, kUserInterface, g_user},
    {kPackageProvider, kPackageInterface, g_package},
    {kPersistentStorageProvider, kPersistentStorageInterface, g_persistentStorage},
    {kDisplayProvider, kDisplayInterface, g_display},
    {kOptionalFeatureProvider, kOptionalFeatureInterface, g_optionalFeature},
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
