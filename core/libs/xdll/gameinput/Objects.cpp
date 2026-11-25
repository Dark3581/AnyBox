#include "xdll/gameinput/Objects.hpp"

#include "xdll/gameinput/Log.hpp"
#include "xdll/gameinput/Sources.hpp"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace abx::gameinput {

namespace {

enum class Interface { Input, Device, Reading };

const char* InterfaceName(Interface which) {
    switch (which) {
    case Interface::Input:
        return "IGameInput";
    case Interface::Device:
        return "IGameInputDevice";
    case Interface::Reading:
        return "IGameInputReading";
    }
    return "?";
}

std::string DescribeAddress(void* address) {
    HMODULE module = nullptr;
    char buffer[MAX_PATH + 64];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &module) != FALSE) {
        char path[MAX_PATH];
        const DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            const char* name = path;
            for (const char* at = path; *at != '\0'; ++at) {
                if (*at == '\\' || *at == '/') name = at + 1;
            }
            std::snprintf(buffer, sizeof(buffer), "%s+0x%llx", name,
                          static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module)));
            return buffer;
        }
    }
    std::snprintf(buffer, sizeof(buffer), "%p", address);
    return buffer;
}

template <Interface Which, std::size_t Slot>
std::uint64_t STDMETHODCALLTYPE Unimplemented(void*) {
#if defined(_MSC_VER)
    void* caller = _ReturnAddress();
#else
    void* caller = __builtin_return_address(0);
#endif
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        Log("[STUB] %s::#%zu first called from %s, returning 0x%llx (E_NOTIMPL)", InterfaceName(Which), Slot, DescribeAddress(caller).c_str(),
            static_cast<unsigned long long>(kUnimplementedResult));
    }
    return kUnimplementedResult;
}

template <Interface Which, std::size_t Count, std::size_t... Slots>
std::array<const void*, Count> UnimplementedTable(std::index_sequence<Slots...>) {
    return {reinterpret_cast<const void*>(&Unimplemented<Which, Slots>)...};
}

template <class Function>
const void* Method(Function function) {
    return reinterpret_cast<const void*>(function);
}

struct Object {
    const void* const* vtable;
};

std::string GuidText(const GUID& id) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", static_cast<unsigned long>(id.Data1), id.Data2, id.Data3, id.Data4[0], id.Data4[1],
                  id.Data4[2], id.Data4[3], id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7]);
    return buffer;
}

bool SameGuid(const GUID& left, const GUID& right) {
    return std::memcmp(&left, &right, sizeof(GUID)) == 0;
}

void NoteRefused(const GUID& id) {
    static std::mutex lock;
    static std::vector<GUID> seen;
    const std::lock_guard guard(lock);
    if (seen.size() >= 16) return;
    for (const GUID& known : seen) {
        if (SameGuid(known, id)) return;
    }
    seen.push_back(id);
    Log("QueryInterface(%s) refused (E_NOINTERFACE)", GuidText(id).c_str());
}

template <class OnSuccess>
HRESULT QueryFor(void* self, const GUID* iid, void** out, const GUID& own, OnSuccess&& onSuccess) {
    if (out == nullptr) return E_POINTER;
    if (iid != nullptr && (SameGuid(*iid, own) || SameGuid(*iid, kIidIUnknown))) {
        onSuccess();
        *out = self;
        return S_OK;
    }
    if (iid != nullptr) NoteRefused(*iid);
    *out = nullptr;
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE InputQueryInterface(Object* self, const GUID* iid, void** out) {
    return QueryFor(self, iid, out, kIidIGameInput, [] {});
}

HRESULT STDMETHODCALLTYPE DeviceQueryInterface(Object* self, const GUID* iid, void** out) {
    return QueryFor(self, iid, out, kIidIGameInputDevice, [] {});
}

ULONG STDMETHODCALLTYPE StaticAddRef(void*) {
    return 1;
}

ULONG STDMETHODCALLTYPE StaticRelease(void*) {
    return 1;
}

void STDMETHODCALLTYPE Nothing(void*) {}

void STDMETHODCALLTYPE NothingWithValue(void*, std::uint32_t) {}

void STDMETHODCALLTYPE NothingWithValueAndGain(void*, std::uint32_t, float) {}

const DeviceInfo* STDMETHODCALLTYPE GetDeviceInfo(Object*) {
    return &GamepadDeviceInfo();
}

std::uint32_t STDMETHODCALLTYPE GetDeviceStatus(Object*) {
    return kDeviceStatus;
}

void STDMETHODCALLTYPE GetBatteryState(Object*, std::array<std::uint32_t, 5>* state) {
    if (state != nullptr) *state = {};
}

bool STDMETHODCALLTYPE GetMotorPoweredOn(Object*, std::uint32_t) {
    return false;
}

void STDMETHODCALLTYPE SetRumbleState(Object*, const std::array<float, 4>* params) {
    if (params == nullptr) {
        SetRumble(0.0f, 0.0f);
        return;
    }
    SetRumble((*params)[0], (*params)[1]);
}

std::uint64_t STDMETHODCALLTYPE GetCurrentTimestamp(Object*) {
    return NowUs();
}

bool Ours(void* device) {
    return device == nullptr || device == DeviceObject();
}

void NoteQuery(const char* what, std::uint32_t kind) {
    static std::mutex lock;
    static std::vector<std::pair<const char*, std::uint32_t>> seen;
    const std::lock_guard guard(lock);
    if (seen.size() >= 32) return;
    for (const auto& [name, known] : seen) {
        if (name == what && known == kind) return;
    }
    seen.emplace_back(what, kind);
    Log("%s for kinds 0x%x first asked", what, kind);
}

constexpr const char* kCurrentReading = "GetCurrentReading";
constexpr const char* kOtherReading = "GetNext/PreviousReading";
constexpr const char* kReadingCallback = "RegisterReadingCallback";

HRESULT STDMETHODCALLTYPE GetCurrentReading(Object*, std::uint32_t kind, void* device, Reading** reading) {
    NoteQuery(kCurrentReading, kind);
    if (reading == nullptr) return E_POINTER;
    *reading = nullptr;
    if ((kind & kKinds) == 0 || !Ours(device)) return kReadingNotFound;
    *reading = NewReading(Poll());
    return S_OK;
}

HRESULT STDMETHODCALLTYPE GetOtherReading(Object*, void*, std::uint32_t kind, void*, Reading** reading) {
    NoteQuery(kOtherReading, kind);
    if (reading != nullptr) *reading = nullptr;
    return kReadingNotFound;
}

HRESULT STDMETHODCALLTYPE GetTemporalReading(Object* self, std::uint64_t, void* device, Reading** reading) {
    return GetCurrentReading(self, kKindGamepad, device, reading);
}

using ReadingCallback = void(STDMETHODCALLTYPE*)(std::uint64_t, void*, Reading*, bool);
using DeviceCallback = void(STDMETHODCALLTYPE*)(std::uint64_t, void*, void*, std::uint64_t, std::uint32_t, std::uint32_t);

std::atomic<std::uint64_t> g_tokens{0};

std::uint64_t NextToken() {
    return g_tokens.fetch_add(1) + 1;
}

class CallbackEntry {
public:
    CallbackEntry(std::uint64_t token, void* context, ReadingCallback callback) : token(token), context(context), callback(callback) {}

    const std::uint64_t token;
    void* const context;
    const ReadingCallback callback;

    bool TryEnter() {
        const std::lock_guard guard(lock_);
        if (stopped_) return false;
        ++inFlight_;
        running_.push_back(std::this_thread::get_id());
        return true;
    }

    void Leave() {
        const std::lock_guard guard(lock_);
        if (inFlight_ > 0) --inFlight_;
        const auto found = std::find(running_.begin(), running_.end(), std::this_thread::get_id());
        if (found != running_.end()) {
            *found = running_.back();
            running_.pop_back();
        }
        done_.notify_all();
    }

    bool StopAndIsIdle() {
        const std::lock_guard guard(lock_);
        stopped_ = true;
        return inFlight_ == 0;
    }

    bool Unregister(std::uint64_t timeoutUs) {
        const std::thread::id me = std::this_thread::get_id();
        std::unique_lock guard(lock_);
        stopped_ = true;
        const auto othersRunning = [&] { return std::any_of(running_.begin(), running_.end(), [&](std::thread::id id) { return id != me; }); };
        if (!othersRunning()) return true;
        if (timeoutUs == 0) return false;
        const auto now = std::chrono::steady_clock::now();
        const auto room = std::chrono::duration_cast<std::chrono::microseconds>((std::chrono::steady_clock::time_point::max)() - now).count();
        if (room <= 0 || timeoutUs >= static_cast<std::uint64_t>(room)) {
            done_.wait(guard, [&] { return !othersRunning(); });
            return true;
        }
        const auto deadline = now + std::chrono::microseconds(static_cast<std::int64_t>(timeoutUs));
        return done_.wait_until(guard, deadline, [&] { return !othersRunning(); });
    }

private:
    std::mutex lock_;
    std::condition_variable done_;
    bool stopped_ = false;
    std::size_t inFlight_ = 0;
    std::vector<std::thread::id> running_;
};

class EntryGuard {
public:
    explicit EntryGuard(CallbackEntry& entry) : entry_(entry) {}
    EntryGuard(const EntryGuard&) = delete;
    EntryGuard& operator=(const EntryGuard&) = delete;
    ~EntryGuard() { entry_.Leave(); }

private:
    CallbackEntry& entry_;
};

struct Registry {
    std::mutex lock;
    std::vector<std::shared_ptr<CallbackEntry>> entries;
};

Registry& Callbacks() {
    static Registry registry;
    return registry;
}

void AddCallback(std::shared_ptr<CallbackEntry> entry) {
    Registry& registry = Callbacks();
    const std::lock_guard guard(registry.lock);
    registry.entries.push_back(std::move(entry));
}

std::shared_ptr<CallbackEntry> FindCallback(std::uint64_t token) {
    Registry& registry = Callbacks();
    const std::lock_guard guard(registry.lock);
    for (const auto& entry : registry.entries) {
        if (entry->token == token) return entry;
    }
    return nullptr;
}

void RemoveCallback(std::uint64_t token) {
    Registry& registry = Callbacks();
    const std::lock_guard guard(registry.lock);
    std::erase_if(registry.entries, [&](const std::shared_ptr<CallbackEntry>& entry) { return entry->token == token; });
}

void StartPoller() {
    static std::once_flag started;
    std::call_once(started, [] {
        try {
            std::thread([] {
                GamepadState last;
                for (;;) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(4));
                    const GamepadState state = Poll();
                    if (state == last) continue;
                    last = state;
                    DispatchReadingCallbacks(state);
                }
            }).detach();
        } catch (const std::system_error& error) {
            Log("the reading callback thread did not start (%s): reading callbacks are not called", error.what());
        }
    });
}

HRESULT STDMETHODCALLTYPE RegisterReadingCallback(Object*, void* device, std::uint32_t kind, float, void* context, ReadingCallback callback, std::uint64_t* token) {
    NoteQuery(kReadingCallback, kind);
    const std::uint64_t issued = NextToken();
    if (token != nullptr) *token = issued;
    if (callback != nullptr && (kind & kKinds) != 0 && Ours(device)) {
        AddCallback(std::make_shared<CallbackEntry>(issued, context, callback));
        StartPoller();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE RegisterDeviceCallback(Object*, void* device, std::uint32_t kind, std::uint32_t, std::uint32_t enumeration, void* context, DeviceCallback callback,
                                                 std::uint64_t* token) {
    const std::uint64_t issued = NextToken();
    if (token != nullptr) *token = issued;
    Log("device callback for kinds 0x%x (enumeration %u)", kind, enumeration);
    if (callback == nullptr || (kind & kKinds) == 0 || !Ours(device) || enumeration == 0) return S_OK;
    if (enumeration == kEnumerationBlocking) {
        callback(issued, context, DeviceObject(), NowUs(), kDeviceStatus, 0);
        return S_OK;
    }
    auto entry = std::make_shared<CallbackEntry>(issued, context, nullptr);
    AddCallback(entry);
    try {
        std::thread([entry, issued, context, callback] {
            if (entry->TryEnter()) {
                const EntryGuard guard(*entry);
                callback(issued, context, DeviceObject(), NowUs(), kDeviceStatus, 0);
            }
            RemoveCallback(issued);
        }).detach();
    } catch (const std::system_error&) {
        RemoveCallback(issued);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE RegisterSystemButtonCallback(Object*, void*, std::uint32_t, void*, void*, std::uint64_t* token) {
    if (token != nullptr) *token = NextToken();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE RegisterKeyboardLayoutCallback(Object*, void*, void*, void*, std::uint64_t* token) {
    if (token != nullptr) *token = NextToken();
    return S_OK;
}

void STDMETHODCALLTYPE StopCallback(Object*, std::uint64_t token) {
    const std::shared_ptr<CallbackEntry> entry = FindCallback(token);
    if (entry != nullptr && entry->StopAndIsIdle()) RemoveCallback(token);
}

bool STDMETHODCALLTYPE UnregisterCallback(Object*, std::uint64_t token, std::uint64_t timeoutUs) {
    const std::shared_ptr<CallbackEntry> entry = FindCallback(token);
    if (entry == nullptr) return true;
    if (!entry->Unregister(timeoutUs)) return false;
    RemoveCallback(token);
    return true;
}

HRESULT STDMETHODCALLTYPE FindDeviceFromId(Object*, const std::array<std::uint8_t, 32>* id, void** device) {
    if (device == nullptr) return E_POINTER;
    if (id != nullptr && *id == kDeviceId) {
        *device = DeviceObject();
        return S_OK;
    }
    *device = nullptr;
    return kDeviceNotFound;
}

HRESULT STDMETHODCALLTYPE EnableOemDeviceSupport(Object*, std::uint16_t, std::uint16_t, std::uint8_t, std::uint8_t) {
    return S_OK;
}

void STDMETHODCALLTYPE SetFocusPolicy(Object*, std::uint32_t) {}

ULONG STDMETHODCALLTYPE ReadingAddRef(Reading* self) {
    return self->references.fetch_add(1, std::memory_order_acq_rel) + 1;
}

ULONG STDMETHODCALLTYPE ReadingRelease(Reading* self) {
    const std::uint32_t left = self->references.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) delete self;
    return left;
}

HRESULT STDMETHODCALLTYPE ReadingQueryInterface(Reading* self, const GUID* iid, void** out) {
    return QueryFor(self, iid, out, kIidIGameInputReading, [self] { ReadingAddRef(self); });
}

std::uint32_t STDMETHODCALLTYPE ReadingKind(Reading*) {
    return kKinds;
}

std::uint64_t STDMETHODCALLTYPE ReadingSequenceNumber(Reading* self, std::uint32_t) {
    return self->sequence;
}

std::uint64_t STDMETHODCALLTYPE ReadingTimestamp(Reading* self) {
    return self->timestamp;
}

void STDMETHODCALLTYPE ReadingDevice(Reading*, void** device) {
    if (device != nullptr) *device = DeviceObject();
}

const char* ReadingMethod(std::size_t slot) {
    switch (slot) {
    case 7:
        return "GetRawReport";
    case 8:
        return "GetControllerAxisCount";
    case 9:
        return "GetControllerAxisState";
    case 10:
        return "GetControllerButtonCount";
    case 11:
        return "GetControllerButtonState";
    case 12:
        return "GetControllerSwitchCount";
    case 13:
        return "GetControllerSwitchState";
    case 14:
        return "GetKeyCount";
    case 15:
        return "GetKeyState";
    case 16:
        return "GetMouseState";
    case 17:
        return "GetTouchCount";
    case 18:
        return "GetTouchState";
    case 19:
        return "GetMotionState";
    case 20:
        return "GetArcadeStickState";
    case 21:
        return "GetFlightStickState";
    case 23:
        return "GetRacingWheelState";
    default:
        return "?";
    }
}

void NoteReadingCall(std::size_t slot) {
    static std::atomic<std::uint32_t> seen{0};
    const std::uint32_t bit = 1u << slot;
    if ((seen.fetch_or(bit) & bit) == 0) Log("IGameInputReading::%s first called (answered: none)", ReadingMethod(slot));
}

template <std::size_t SlotIndex>
std::uint32_t STDMETHODCALLTYPE ZeroCount(Reading*, std::uint32_t, void*) {
    NoteReadingCall(SlotIndex);
    return 0;
}

template <std::size_t SlotIndex>
bool STDMETHODCALLTYPE NoState(Reading*, void*) {
    NoteReadingCall(SlotIndex);
    return false;
}

bool STDMETHODCALLTYPE ReadingGamepadState(Reading* self, GamepadState* state) {
    if (state == nullptr) return false;
    *state = self->state;
    return true;
}

bool STDMETHODCALLTYPE ReadingUiNavigationState(Reading* self, std::uint32_t* state) {
    if (state == nullptr) return false;
    *state = UiNavigation(self->state.buttons);
    return true;
}

const std::array<const void*, kInputSlots>& InputTable() {
    static const std::array<const void*, kInputSlots> table = [] {
        auto built = UnimplementedTable<Interface::Input, kInputSlots>(std::make_index_sequence<kInputSlots>{});
        built[0] = Method(&InputQueryInterface);
        built[1] = Method(&StaticAddRef);
        built[2] = Method(&StaticRelease);
        built[3] = Method(&GetCurrentTimestamp);
        built[4] = Method(&GetCurrentReading);
        built[5] = Method(&GetOtherReading);
        built[6] = Method(&GetOtherReading);
        built[7] = Method(&GetTemporalReading);
        built[8] = Method(&RegisterReadingCallback);
        built[9] = Method(&RegisterDeviceCallback);
        built[10] = Method(&RegisterSystemButtonCallback);
        built[11] = Method(&RegisterKeyboardLayoutCallback);
        built[12] = Method(&StopCallback);
        built[13] = Method(&UnregisterCallback);
        built[16] = Method(&FindDeviceFromId);
        built[20] = Method(&EnableOemDeviceSupport);
        built[21] = Method(&SetFocusPolicy);
        return built;
    }();
    return table;
}

const std::array<const void*, kDeviceSlots>& DeviceTable() {
    static const std::array<const void*, kDeviceSlots> table = [] {
        auto built = UnimplementedTable<Interface::Device, kDeviceSlots>(std::make_index_sequence<kDeviceSlots>{});
        built[0] = Method(&DeviceQueryInterface);
        built[1] = Method(&StaticAddRef);
        built[2] = Method(&StaticRelease);
        built[3] = Method(&GetDeviceInfo);
        built[4] = Method(&GetDeviceStatus);
        built[5] = Method(&GetBatteryState);
        built[7] = Method(&GetMotorPoweredOn);
        built[8] = Method(&NothingWithValueAndGain);
        built[10] = Method(&SetRumbleState);
        built[11] = Method(&NothingWithValue);
        built[12] = Method(&Nothing);
        built[13] = Method(&Nothing);
        return built;
    }();
    return table;
}

const std::array<const void*, kReadingSlots>& ReadingTable() {
    static const std::array<const void*, kReadingSlots> table = [] {
        auto built = UnimplementedTable<Interface::Reading, kReadingSlots>(std::make_index_sequence<kReadingSlots>{});
        built[0] = Method(&ReadingQueryInterface);
        built[1] = Method(&ReadingAddRef);
        built[2] = Method(&ReadingRelease);
        built[3] = Method(&ReadingKind);
        built[4] = Method(&ReadingSequenceNumber);
        built[5] = Method(&ReadingTimestamp);
        built[6] = Method(&ReadingDevice);
        built[7] = Method(&NoState<7>);
        built[8] = Method(&ZeroCount<8>);
        built[9] = Method(&ZeroCount<9>);
        built[10] = Method(&ZeroCount<10>);
        built[11] = Method(&ZeroCount<11>);
        built[12] = Method(&ZeroCount<12>);
        built[13] = Method(&ZeroCount<13>);
        built[14] = Method(&ZeroCount<14>);
        built[15] = Method(&ZeroCount<15>);
        built[16] = Method(&NoState<16>);
        built[17] = Method(&ZeroCount<17>);
        built[18] = Method(&ZeroCount<18>);
        built[19] = Method(&NoState<19>);
        built[20] = Method(&NoState<20>);
        built[21] = Method(&NoState<21>);
        built[22] = Method(&ReadingGamepadState);
        built[23] = Method(&NoState<23>);
        built[24] = Method(&ReadingUiNavigationState);
        return built;
    }();
    return table;
}

std::atomic<std::uint64_t> g_sequence{0};

}

void* InputObject() {
    static Object object{InputTable().data()};
    return &object;
}

void* DeviceObject() {
    static Object object{DeviceTable().data()};
    return &object;
}

const DeviceInfo& GamepadDeviceInfo() {
    static const DeviceInfo info = [] {
        DeviceInfo built{};
        built.infoSize = sizeof(DeviceInfo);
        built.vendorId = 0x045E;
        built.productId = 0x0B12;
        built.revisionNumber = 1;
        built.usagePage = 1;
        built.usageId = 5;
        built.hardwareVersion = {1, 0, 0, 0};
        built.firmwareVersion = {1, 0, 0, 0};
        built.deviceId = kDeviceId;
        built.deviceRootId = kDeviceId;
        built.deviceFamily = 1;
        built.capabilities = 8;
        built.supportedInput = kKinds;
        built.supportedRumbleMotors = 0xF;
        return built;
    }();
    return info;
}

Reading* NewReading(const GamepadState& state) {
    auto* reading = new Reading;
    reading->vtable = ReadingTable().data();
    reading->references.store(1);
    reading->state = state;
    reading->sequence = g_sequence.fetch_add(1) + 1;
    reading->timestamp = NowUs();
    return reading;
}

void DispatchReadingCallbacks(const GamepadState& state) {
    std::vector<std::shared_ptr<CallbackEntry>> entries;
    {
        Registry& registry = Callbacks();
        const std::lock_guard guard(registry.lock);
        for (const auto& entry : registry.entries) {
            if (entry->callback != nullptr) entries.push_back(entry);
        }
    }
    for (const auto& entry : entries) {
        if (!entry->TryEnter()) continue;
        const EntryGuard guard(*entry);
        Reading* reading = NewReading(state);
        entry->callback(entry->token, entry->context, reading, false);
        ReadingRelease(reading);
    }
}

HRESULT Create(void** gameInput) {
    if (gameInput == nullptr) return E_POINTER;
    NowUs();
    static std::once_flag started;
    std::call_once(started, StartSources);
    *gameInput = InputObject();
    Log("GameInputCreate: one gamepad (XInput controllers, keyboard, pipe)");
    return S_OK;
}

}
