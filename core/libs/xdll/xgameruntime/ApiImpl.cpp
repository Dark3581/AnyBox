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
HRESULT ThreadingAsyncGetResultSize(ApiObject*, XAsyncBlock* block, std::size_t* size) { return XAsyncGetResultSize(block, size); }
HRESULT ThreadingAsyncRun(ApiObject*, XAsyncBlock* block, XAsyncWork* work) { return XAsyncRun(block, work); }
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
bool ThreadingQueueGetProcessQueue(ApiObject*, XTaskQueueHandle* queue) { return XTaskQueueGetCurrentProcessTaskQueue(queue); }
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
    Entry(&ThreadingAsyncGetResultSize),
    Entry(&ThreadingAsyncCancel),
    Entry(&ThreadingAsyncRun),
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
    Entry(&ThreadingQueueGetProcessQueue),
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

HRESULT UserAddAsync(ApiObject*, std::uint32_t options, XAsyncBlock* async) {
    return XUserAddAsync(options, async);
}

HRESULT UserAddResult(ApiObject*, XAsyncBlock* async, XUserHandle* user) {
    return XUserAddResult(async, user);
}

void UserCloseHandle(ApiObject*, XUserHandle user) {
    AbxUserCloseHandle(user);
}

HRESULT UserGetIsGuest(ApiObject*, XUserHandle user, bool* isGuest) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("xgameruntime", "user slot 13 writes one bool for the user (inferred to be XUserGetIsGuest from its shape and use as a flag); the offline user answers false");
    }
    return XUserGetIsGuest(user, isGuest);
}

HRESULT UserCheckPrivilege(ApiObject*, XUserHandle user, std::uint32_t options, std::uint32_t privilege, bool* hasPrivilege, std::uint32_t* reasons) {
    return XUserCheckPrivilege(user, options, privilege, hasPrivilege, reasons);
}

HRESULT UserGetAgeGroup(ApiObject*, XUserHandle user, std::uint32_t* ageGroup) {
    return XUserGetAgeGroup(user, ageGroup);
}

HRESULT UserDuplicateHandle(ApiObject*, XUserHandle user, XUserHandle* duplicated) {
    return AbxUserDuplicateHandle(user, duplicated);
}

template <std::size_t Slot>
HRESULT UserIdentifier(ApiObject*, XUserHandle user, std::uint64_t* value) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("xgameruntime", "user slot %zu returns a 64-bit identifier of the user; slots 9 and 11 both answer the offline user's id 1, since which is the XUID and which the local id is not measured", Slot);
    }
    return AbxUserGetIdentifier(user, value);
}

const void* const kUserTable[] = {
    Entry(&ObjectQueryInterface),
    Entry(&ObjectAddRef),
    Entry(&ObjectRelease),
    Entry(&UserDuplicateHandle),
    Entry(&UserCloseHandle),
    Entry(&Unmapped<5>),
    Entry(&Unmapped<6>),
    Entry(&UserAddAsync),
    Entry(&UserAddResult),
    Entry(&UserIdentifier<9>),
    Entry(&Unmapped<10>),
    Entry(&UserIdentifier<11>),
    Entry(&Unmapped<12>),
    Entry(&UserGetIsGuest),
    Entry(&Unmapped<14>),
    Entry(&Unmapped<15>),
    Entry(&Unmapped<16>),
    Entry(&Unmapped<17>),
    Entry(&Unmapped<18>),
    Entry(&UserGetAgeGroup),
    Entry(&UserCheckPrivilege),
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

constexpr GUID kFeatureProvider{0x8836fe87, 0xedb9, 0x4fe3, {0x8d, 0xad, 0x05, 0xf0, 0xd2, 0xcd, 0x5b, 0x40}};

bool FeatureIsAvailable(ApiObject*, std::uint32_t feature) {
    const bool available = feature == 0 || (feature >= 2 && feature <= 13) || (feature >= 15 && feature <= 21);
    static std::atomic<std::uint32_t> reported{0};
    if (feature < 32 && (reported.fetch_or(1u << feature) & (1u << feature)) == 0) {
        AbxLog("xgameruntime", "XGameRuntimeIsFeatureAvailable(%u) -> %s", feature, available ? "true" : "false (AnyBox refuses that API)");
    }
    return available;
}

const void* const kFeatureTable[] = {
    Entry(&ObjectQueryInterface),
    Entry(&ObjectAddRef),
    Entry(&ObjectRelease),
    Entry(&FeatureIsAvailable),
};

ApiObject g_feature{kFeatureTable, &kFeatureProvider, "feature availability"};

constexpr GUID kLaunchProvider{0x1b339674, 0x328d, 0x4283, {0xa2, 0x00, 0x31, 0x71, 0xf1, 0x8d, 0x36, 0x39}};

HRESULT LaunchUri(ApiObject*, void* user, const char* uri) {
    static_cast<void>(user);
    AbxLog("xgameruntime", "XLaunchUri(%s): refused with E_NOTIMPL, as opening a link for the title would act on the host", uri != nullptr ? uri : "(null)");
    return E_NOTIMPL;
}

HRESULT DisplayAcquireTimeoutDeferral(ApiObject*, void** handle) {
    if (handle == nullptr) return E_INVALIDARG;
    static std::atomic<std::uintptr_t> next{0};
    *handle = reinterpret_cast<void*>(++next);
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("xgameruntime", "XDisplayAcquireTimeoutDeferral: the title gets a deferral handle; AnyBox does not change the host's display timeout (slots 3 to 5 of this provider "
                               "are XLaunchUri, acquire and close, as xcl maps them)");
    }
    return S_OK;
}

void DisplayCloseTimeoutDeferralHandle(ApiObject*, void* handle) {
    static_cast<void>(handle);
}

const void* const kLaunchTable[] = {
    Entry(&ObjectQueryInterface),
    Entry(&ObjectAddRef),
    Entry(&ObjectRelease),
    Entry(&LaunchUri),
    Entry(&DisplayAcquireTimeoutDeferral),
    Entry(&DisplayCloseTimeoutDeferralHandle),
};

ApiObject g_launch{kLaunchTable, &kLaunchProvider, "launch and display timeout"};

constexpr GUID kGameEventProvider{0xbbfbdcc7, 0xbfe7, 0x409b, {0xa5, 0xca, 0xed, 0xf0, 0x54, 0x96, 0x0b, 0x4d}};

HRESULT GameEventWrite(ApiObject*, void* user, const char* serviceConfigurationId, const char* playSessionId, const char* name, const char* dimensions, const char* measurements) {
    static_cast<void>(user);
    static_cast<void>(serviceConfigurationId);
    static_cast<void>(playSessionId);
    static_cast<void>(dimensions);
    static_cast<void>(measurements);
    static std::atomic<int> reported{0};
    if (reported.fetch_add(1) < 8) {
        AbxLog("xgameruntime", "XGameEventWrite(%s): telemetry is accepted and dropped (AnyBox is offline)", name != nullptr ? name : "(null)");
    }
    return S_OK;
}

const void* const kGameEventTable[] = {
    Entry(&ObjectQueryInterface),
    Entry(&ObjectAddRef),
    Entry(&ObjectRelease),
    Entry(&GameEventWrite),
};

ApiObject g_gameEvent{kGameEventTable, &kGameEventProvider, "game events"};

constexpr GUID kErrorProvider{0x8ca467f7, 0x22e8, 0x4096, {0x84, 0x56, 0xbb, 0x8a, 0xa1, 0x3f, 0x79, 0xd8}};

using ErrorCallback = bool (*)(HRESULT, const char*, void*);
std::atomic<ErrorCallback> g_errorCallback{nullptr};
std::atomic<void*> g_errorContext{nullptr};

void ErrorReport(ApiObject*, HRESULT result, const char* message) {
    AbxLog("xgameruntime", "XErrorReport 0x%08lx: %s", static_cast<unsigned long>(result), message != nullptr ? message : "(no message)");
    if (const ErrorCallback callback = g_errorCallback.load()) callback(result, message, g_errorContext.load());
}

HRESULT ErrorSetCallback(ApiObject*, ErrorCallback callback, void* context) {
    g_errorContext.store(context);
    g_errorCallback.store(callback);
    return S_OK;
}

HRESULT ErrorSetOptions(ApiObject*, std::uint32_t debuggerOptions, std::uint32_t eventOptions) {
    AbxLog("xgameruntime", "XErrorSetOptions(0x%x, 0x%x): recorded; AnyBox reports errors to the log", debuggerOptions, eventOptions);
    return S_OK;
}

const void* const kErrorTable[] = {
    Entry(&ObjectQueryInterface),
    Entry(&ObjectAddRef),
    Entry(&ObjectRelease),
    Entry(&ErrorReport),
    Entry(&ErrorSetCallback),
    Entry(&ErrorSetOptions),
};

ApiObject g_error{kErrorTable, &kErrorProvider, "error reporting"};

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

constexpr GUID kGameProvider{0x973a344e, 0x24bf, 0x4d0f, {0x84, 0x57, 0x56, 0xc5, 0x34, 0x89, 0x2b, 0x29}};
constexpr GUID kStoreProvider{0x0dd112ac, 0x7c24, 0x448c, {0xb9, 0x2b, 0x39, 0x60, 0xfb, 0x5b, 0xd3, 0x0c}};
constexpr GUID kAppBroadcastProvider{0x186d5592, 0xa72d, 0x45fb, {0x95, 0x60, 0x11, 0xae, 0xd0, 0xe6, 0x64, 0x7a}};
constexpr GUID kAppCaptureProvider{0xa4f1aee2, 0x4bf1, 0x4485, {0xb0, 0x08, 0xa7, 0xc2, 0x6d, 0x52, 0xac, 0x27}};

struct UnavailableProvider {
    const GUID& provider;
    const char* name;
};

const UnavailableProvider kUnavailableProviders[] = {
    {kStoreProvider, "XStore"},
    {kAppBroadcastProvider, "XAppBroadcast"},
    {kAppCaptureProvider, "XAppCapture"},
};
constexpr GUID kGameInterface{0x2549f142, 0x6419, 0x4a06, {0x97, 0xb5, 0x93, 0x1a, 0xab, 0x7c, 0x2f, 0x34}};

HRESULT GameGetTitleId(ApiObject*, std::uint32_t* titleId) {
    return XGameGetXboxTitleId(titleId);
}

const void* const kGameTable[] = {
    Entry(&ObjectQueryInterface), Entry(&ObjectAddRef), Entry(&ObjectRelease), Entry(&GameGetTitleId), Entry(&Unmapped<4>),
};

ApiObject g_game{kGameTable, &kGameInterface, "game"};

constexpr GUID kSystemProvider{0xe349bd1a, 0xfc20, 0x4e40, {0xb9, 0x9c, 0x41, 0x78, 0xcc, 0x6b, 0x40, 0x9f}};
constexpr GUID kSystemInterface{0x1861cf2e, 0xe18b, 0x4834, {0xa9, 0xf5, 0xb4, 0xa4, 0xe6, 0xef, 0xb4, 0xcf}};

HRESULT SystemGetSandboxId(ApiObject*, std::size_t size, char* sandbox, std::size_t* used) {
    return XSystemGetXboxLiveSandboxId(size, sandbox, used);
}

HRESULT SystemGetConsoleId(ApiObject*, std::size_t size, char* id, std::size_t* used) {
    return XSystemGetConsoleId(size, id, used);
}

const void* const kSystemTable[] = {
    Entry(&ObjectQueryInterface), Entry(&ObjectAddRef), Entry(&ObjectRelease), Entry(&SystemGetConsoleId), Entry(&SystemGetSandboxId), Entry(&Unmapped<5>),
};

ApiObject g_system{kSystemTable, &kSystemInterface, "system"};

constexpr GUID kGameSaveProvider{0x704c3f58, 0xe629, 0x4cc2, {0xb1, 0x97, 0x30, 0x51, 0x1b, 0x99, 0x6f, 0xe2}};
constexpr GUID kGameSaveInterface{0x1bfff3af, 0xf14a, 0x40a3, {0x8e, 0x35, 0x9a, 0xda, 0x90, 0x65, 0x93, 0xf9}};

HRESULT GameSaveInitializeProviderAsync(ApiObject*, XUserHandle user, const char* configurationId, bool syncOnDemand, XAsyncBlock* async) {
    return XGameSaveInitializeProviderAsync(user, configurationId, syncOnDemand, async);
}

HRESULT GameSaveInitializeProviderResult(ApiObject*, XAsyncBlock* async, XGameSaveProviderHandle* provider);
void GameSaveCloseProvider(ApiObject*, XGameSaveProviderHandle provider);

HRESULT GameSaveGetRemainingQuotaAsync(ApiObject*, XGameSaveProviderHandle provider, XAsyncBlock* async) { return XGameSaveGetRemainingQuotaAsync(provider, async); }
HRESULT GameSaveGetRemainingQuotaResult(ApiObject*, XAsyncBlock* async, std::int64_t* quota) { return XGameSaveGetRemainingQuotaResult(async, quota); }
HRESULT GameSaveDeleteContainerAsync(ApiObject*, XGameSaveProviderHandle provider, const char* name, XAsyncBlock* async) { return XGameSaveDeleteContainerAsync(provider, name, async); }
HRESULT GameSaveDeleteContainerResult(ApiObject*, XAsyncBlock* async) { return XGameSaveDeleteContainerResult(async); }
HRESULT GameSaveGetContainerInfo(ApiObject*, XGameSaveProviderHandle provider, const char* name, void* context, XGameSaveContainerInfoCallback* callback) {
    return XGameSaveGetContainerInfo(provider, name, context, callback);
}
HRESULT GameSaveEnumerateContainerInfo(ApiObject*, XGameSaveProviderHandle provider, void* context, XGameSaveContainerInfoCallback* callback) {
    return XGameSaveEnumerateContainerInfo(provider, context, callback);
}
HRESULT GameSaveCreateContainer(ApiObject*, XGameSaveProviderHandle provider, const char* name, XGameSaveContainerHandle* container) {
    return XGameSaveCreateContainer(provider, name, container);
}
void GameSaveCloseContainer(ApiObject*, XGameSaveContainerHandle container) { XGameSaveCloseContainer(container); }
HRESULT GameSaveEnumerateBlobInfoByName(ApiObject*, XGameSaveContainerHandle container, const char* prefix, void* context, XGameSaveBlobInfoCallback* callback) {
    return XGameSaveEnumerateBlobInfoByName(container, prefix, context, callback);
}
HRESULT GameSaveReadBlobData(ApiObject*, XGameSaveContainerHandle container, const char** names, std::uint32_t* count, std::size_t size, XGameSaveBlob* data) {
    return XGameSaveReadBlobData(container, names, count, size, data);
}
HRESULT GameSaveReadBlobDataAsync(ApiObject*, XGameSaveContainerHandle container, const char** names, std::uint32_t count, XAsyncBlock* async) {
    return XGameSaveReadBlobDataAsync(container, names, count, async);
}
HRESULT GameSaveReadBlobDataResult(ApiObject*, XAsyncBlock* async, std::size_t size, XGameSaveBlob* data, std::uint32_t* count) {
    return XGameSaveReadBlobDataResult(async, size, data, count);
}
HRESULT GameSaveCreateUpdate(ApiObject*, XGameSaveContainerHandle container, const char* displayName, XGameSaveUpdateHandle* update) {
    return XGameSaveCreateUpdate(container, displayName, update);
}
void GameSaveCloseUpdate(ApiObject*, XGameSaveUpdateHandle update) { XGameSaveCloseUpdate(update); }
HRESULT GameSaveSubmitBlobWrite(ApiObject*, XGameSaveUpdateHandle update, const char* name, const std::uint8_t* data, std::size_t bytes) {
    return XGameSaveSubmitBlobWrite(update, name, data, bytes);
}
HRESULT GameSaveSubmitUpdateAsync(ApiObject*, XGameSaveUpdateHandle update, XAsyncBlock* async) { return XGameSaveSubmitUpdateAsync(update, async); }
HRESULT GameSaveSubmitUpdateResult(ApiObject*, XAsyncBlock* async) { return XGameSaveSubmitUpdateResult(async); }

const void* const kGameSaveTable[] = {
    Entry(&ObjectQueryInterface),
    Entry(&ObjectAddRef),
    Entry(&ObjectRelease),
    Entry(&Unmapped<3>),
    Entry(&GameSaveInitializeProviderAsync),
    Entry(&GameSaveInitializeProviderResult),
    Entry(&GameSaveCloseProvider),
    Entry(&Unmapped<7>),
    Entry(&GameSaveGetRemainingQuotaAsync),
    Entry(&GameSaveGetRemainingQuotaResult),
    Entry(&Unmapped<10>),
    Entry(&GameSaveDeleteContainerAsync),
    Entry(&GameSaveDeleteContainerResult),
    Entry(&GameSaveGetContainerInfo),
    Entry(&GameSaveEnumerateContainerInfo),
    Entry(&Unmapped<15>),
    Entry(&GameSaveCreateContainer),
    Entry(&GameSaveCloseContainer),
    Entry(&Unmapped<18>),
    Entry(&GameSaveEnumerateBlobInfoByName),
    Entry(&GameSaveReadBlobData),
    Entry(&GameSaveReadBlobDataAsync),
    Entry(&GameSaveReadBlobDataResult),
    Entry(&GameSaveCreateUpdate),
    Entry(&GameSaveCloseUpdate),
    Entry(&GameSaveSubmitBlobWrite),
    Entry(&Unmapped<26>),
    Entry(&Unmapped<27>),
    Entry(&GameSaveSubmitUpdateAsync),
    Entry(&GameSaveSubmitUpdateResult),
};

ApiObject g_gameSave{kGameSaveTable, &kGameSaveInterface, "game save"};

constexpr GUID kUserProfileInterface{0xcef4fac0, 0x7676, 0x4a94, {0xa1, 0x19, 0x4c, 0x43, 0xf9, 0xeb, 0x5b, 0x74}};

HRESULT UserGetGamertag(ApiObject*, XUserHandle user, std::uint32_t component, std::size_t size, char* gamertag, std::size_t* used) {
    return XUserGetGamertag(user, component, size, gamertag, used);
}

const void* const kUserProfileTable[] = {
    Entry(&ObjectQueryInterface), Entry(&ObjectAddRef), Entry(&ObjectRelease), Entry(&UserGetGamertag),
};

ApiObject g_userProfile{kUserProfileTable, &kUserProfileInterface, "user gamertag"};

HRESULT GameSaveInitializeProviderResult(ApiObject*, XAsyncBlock* async, XGameSaveProviderHandle* provider) {
    return XGameSaveInitializeProviderResult(async, provider);
}

void GameSaveCloseProvider(ApiObject*, XGameSaveProviderHandle provider) {
    XGameSaveCloseProvider(provider);
}

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
    {kFeatureProvider, kFeatureProvider, g_feature},
    {kGameEventProvider, kGameEventProvider, g_gameEvent},
    {kLaunchProvider, kLaunchProvider, g_launch},
    {kErrorProvider, kErrorProvider, g_error},
    {kOptionalFeatureProvider, kOptionalFeatureInterface, g_optionalFeature},
    {kGameProvider, kGameInterface, g_game},
    {kSystemProvider, kSystemInterface, g_system},
    {kGameSaveProvider, kGameSaveInterface, g_gameSave},
    {kUserInterface, kUserProfileInterface, g_userProfile},
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
    for (const UnavailableProvider& unavailable : kUnavailableProviders) {
        if (!Same(providerId, unavailable.provider)) continue;
        static std::atomic<std::uint32_t> reported{0};
        const std::uint32_t bit = 1u << static_cast<std::uint32_t>(&unavailable - kUnavailableProviders);
        if ((reported.fetch_or(bit) & bit) == 0) {
            AbxLog("xgameruntime", "QueryApiImpl: %s (interface %s) answers REGDB_E_CLASSNOTREG: the service is not available offline, as on a console without it",
                   unavailable.name, GuidText(interfaceId).c_str());
        }
        return REGDB_E_CLASSNOTREG;
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
