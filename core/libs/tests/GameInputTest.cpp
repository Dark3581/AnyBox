#include "xdll/gameinput/Gamepad.hpp"
#include "xdll/gameinput/Injection.hpp"
#include "xdll/gameinput/Log.hpp"
#include "xdll/gameinput/Objects.hpp"
#include "xdll/gameinput/Sources.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace abx::gameinput;

int g_failures = 0;

void Expect(bool condition, const std::string& what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

std::mutex g_logLock;
std::vector<std::string> g_log;

void CaptureLog(const char* line) {
    const std::lock_guard guard(g_logLock);
    g_log.emplace_back(line);
    std::printf("[gameinput] %s\n", line);
}

std::size_t LogLinesContaining(const std::string& text) {
    const std::lock_guard guard(g_logLock);
    std::size_t count = 0;
    for (const std::string& line : g_log) {
        if (line.find(text) != std::string::npos) ++count;
    }
    return count;
}

template <class R, class... Args>
R Call(void* object, std::size_t slot, Args... args) {
    const void* const* table = *static_cast<const void* const* const*>(object);
    using Function = R(STDMETHODCALLTYPE*)(void*, Args...);
    return reinterpret_cast<Function>(const_cast<void*>(table[slot]))(object, args...);
}

HRESULT QueryInterface(void* object, const GUID* iid, void** out) {
    return Call<HRESULT>(object, 0, iid, out);
}

std::uint32_t References(Reading* reading) {
    return reading->references.load(std::memory_order_acquire);
}

void XInputButtonsMapToGameInput() {
    Expect(ButtonsFromXInput(0x1000 | 0x2000 | 0x0001 | 0x0010) == (kA | kB | kDPadUp | kMenu), "XInput A+B+up+start map to A|B|DPadUp|Menu");
    Expect(UiNavigation(kA | kB | kDPadLeft) == (0x4u | 0x8u | 0x40u), "UI navigation of A|B|DPadLeft");
    Expect(offsetof(DeviceInfo, supportedInput) == 104, "DeviceInfo.supportedInput at 104");
}

void XInputPadsMerge() {
    std::array<XInputPad, 2> pads{};
    pads[0].buttons = 0x1000;
    pads[0].leftThumbX = 32767;
    pads[0].leftThumbY = -32768;
    pads[0].rightTrigger = 255;
    pads[1].buttons = 0x0002;
    pads[1].leftThumbX = -32767;
    pads[1].leftTrigger = 255;
    const GamepadState merged = GamepadFromXInput(pads);
    Expect(merged.buttons == (kA | kDPadDown), "buttons of every connected pad are ORed");
    Expect(merged.leftThumbstickX == 1.0f && merged.leftThumbstickY == -1.0f, "sticks come from the first connected pad, -32768 clamps to -1");
    Expect(merged.rightTrigger == 1.0f && merged.leftTrigger == 0.0f, "triggers come from the first connected pad");
    Expect(GamepadFromXInput({}) == GamepadState{}, "no connected pad is the resting state");
}

void ReadingsComeAndGo() {
    void* input = InputObject();
    Reading* reading = nullptr;
    Expect(Call<HRESULT>(input, 4, kKindGamepad, static_cast<void*>(nullptr), &reading) == S_OK, "GetCurrentReading(gamepad) succeeds");
    if (reading == nullptr) return;
    GamepadState state;
    state.buttons = 99;
    Expect(Call<bool>(reading, 22, &state), "GetGamepadState answers");
    Expect(state.buttons != 99, "GetGamepadState writes the state");
    void* device = nullptr;
    Call<void>(reading, 6, &device);
    Expect(device == DeviceObject(), "GetDevice is the gamepad");
    Expect(Call<ULONG>(reading, 2) == 0, "the last Release frees the reading");
    reading = reinterpret_cast<Reading*>(0x1);
    Expect(Call<HRESULT>(input, 4, std::uint32_t{0x10}, static_cast<void*>(nullptr), &reading) == kReadingNotFound, "keyboard kind is not found");
    Expect(reading == nullptr, "a reading that is not found is null");
}

void PressScriptParsesButtonsAndTimes() {
    std::vector<std::string> rejected;
    const std::vector<Press> presses = ParsePresses("14:A, 18.5:down,20:Rb,junk,3:NOPE", "1,2.5", rejected);
    const std::vector<Press> expected{{1.0, kA}, {2.5, kA}, {14.0, kA}, {18.5, kDPadDown}, {20.0, kRightShoulder}};
    Expect(presses == expected, "press script entries and order");
    Expect(rejected == std::vector<std::string>{"junk", "3:NOPE"}, "rejected press script entries are reported");
    rejected.clear();
    Expect(ParsePresses("", "", rejected).empty() && rejected.empty(), "empty press scripts");
    Expect(ScriptedButtons(expected, 14.1) == kA, "a press holds for 200 ms");
    Expect(ScriptedButtons(expected, 14.2) == 0, "a press ends after 200 ms");
    Expect(ScriptedButtons(expected, 18.6) == kDPadDown, "a named press");
}

void FocusGatesKeyboardButKeepsScriptedAndInjected() {
    GamepadState state;
    state.buttons = kB;
    state.leftThumbstickX = 0.25f;
    bool keyboardCalled = false;
    bool scriptedCalled = false;
    bool injectedCalled = false;
    MergeHostInputs(
        state, false,
        [&] {
            keyboardCalled = true;
            return kMenu;
        },
        [&] {
            scriptedCalled = true;
            return kA;
        },
        [&](GamepadState& merged) {
            injectedCalled = true;
            merged.buttons |= kX;
        });
    Expect(!keyboardCalled, "the keyboard is not read without focus");
    Expect(scriptedCalled, "scripted presses are read without focus");
    Expect(injectedCalled, "injected input applies without focus");
    Expect(state.buttons == (kB | kA | kX), "unfocused merge keeps the pad, scripted and injected buttons");
    Expect(state.leftThumbstickX == 0.25f, "unfocused merge keeps the stick");

    GamepadState focused;
    MergeHostInputs(focused, true, [] { return kMenu; }, [] { return kA; }, [](GamepadState& merged) { merged.leftThumbstickX = -1.0f; });
    Expect(focused.buttons == (kMenu | kA), "focused merge adds the keyboard");
    Expect(focused.leftThumbstickX == -1.0f, "focused merge applies injected sticks");
}

void QueryInterfaceValidatesIidsAndRefcounts() {
    const GUID unknownIid = {0xdeadbeef, 0x1234, 0x5678, {0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78}};
    void* const sentinel = reinterpret_cast<void*>(0x1234);

    void* input = InputObject();
    void* out = sentinel;
    Expect(QueryInterface(input, &kIidIUnknown, &out) == S_OK && out == input, "IGameInput answers IUnknown");
    out = sentinel;
    Expect(QueryInterface(input, &kIidIGameInput, &out) == S_OK && out == input, "IGameInput answers IGameInput");
    for (const GUID* bad : {&kIidIGameInputDevice, &kIidIGameInputReading, &unknownIid}) {
        out = sentinel;
        Expect(QueryInterface(input, bad, &out) == E_NOINTERFACE && out == nullptr, "IGameInput refuses other IIDs and zeroes *out");
    }
    out = sentinel;
    Expect(QueryInterface(input, nullptr, &out) == E_NOINTERFACE && out == nullptr, "IGameInput refuses a null IID");
    Expect(QueryInterface(input, &kIidIGameInput, nullptr) == E_POINTER, "IGameInput QueryInterface with null out");

    void* device = DeviceObject();
    out = sentinel;
    Expect(QueryInterface(device, &kIidIUnknown, &out) == S_OK && out == device, "IGameInputDevice answers IUnknown");
    out = sentinel;
    Expect(QueryInterface(device, &kIidIGameInputDevice, &out) == S_OK && out == device, "IGameInputDevice answers IGameInputDevice");
    for (const GUID* bad : {&kIidIGameInput, &kIidIGameInputReading, &unknownIid}) {
        out = sentinel;
        Expect(QueryInterface(device, bad, &out) == E_NOINTERFACE && out == nullptr, "IGameInputDevice refuses other IIDs");
    }
    out = sentinel;
    Expect(QueryInterface(device, nullptr, &out) == E_NOINTERFACE && out == nullptr, "IGameInputDevice refuses a null IID");
    Expect(QueryInterface(device, &kIidIGameInputDevice, nullptr) == E_POINTER, "IGameInputDevice QueryInterface with null out");

    Reading* reading = NewReading(GamepadState{});
    out = sentinel;
    Expect(QueryInterface(reading, &kIidIUnknown, &out) == S_OK && out == reading, "IGameInputReading answers IUnknown");
    Expect(References(reading) == 2, "QueryInterface(IUnknown) adds a reference");
    out = sentinel;
    Expect(QueryInterface(reading, &kIidIGameInputReading, &out) == S_OK && out == reading, "IGameInputReading answers IGameInputReading");
    Expect(References(reading) == 3, "QueryInterface(IGameInputReading) adds a reference");
    for (const GUID* bad : {&kIidIGameInput, &kIidIGameInputDevice, &unknownIid}) {
        out = sentinel;
        Expect(QueryInterface(reading, bad, &out) == E_NOINTERFACE && out == nullptr, "IGameInputReading refuses other IIDs");
        Expect(References(reading) == 3, "a failed QueryInterface adds no reference");
    }
    out = sentinel;
    Expect(QueryInterface(reading, nullptr, &out) == E_NOINTERFACE && out == nullptr && References(reading) == 3, "IGameInputReading refuses a null IID");
    Expect(QueryInterface(reading, &kIidIGameInputReading, nullptr) == E_POINTER && References(reading) == 3, "IGameInputReading QueryInterface with null out");
    Expect(Call<ULONG>(reading, 2) == 2, "Release to 2");
    Expect(Call<ULONG>(reading, 2) == 1, "Release to 1");
    Expect(Call<ULONG>(reading, 2) == 0, "Release to 0");
}

struct CallbackGate {
    std::mutex lock;
    std::condition_variable changed;
    bool entered = false;
    bool proceed = false;
    std::atomic<std::uint32_t> calls{0};
    std::atomic<bool> finished{false};

    void OnCallback() {
        if (calls.fetch_add(1, std::memory_order_acq_rel) != 0) return;
        std::unique_lock guard(lock);
        entered = true;
        changed.notify_all();
        changed.wait(guard, [&] { return proceed; });
        finished.store(true, std::memory_order_release);
    }

    void WaitEntered() {
        std::unique_lock guard(lock);
        changed.wait(guard, [&] { return entered; });
    }

    void Release() {
        const std::lock_guard guard(lock);
        proceed = true;
        changed.notify_all();
    }
};

void STDMETHODCALLTYPE GatedReadingCallback(std::uint64_t, void* context, Reading*, bool) {
    static_cast<CallbackGate*>(context)->OnCallback();
}

void STDMETHODCALLTYPE GatedDeviceCallback(std::uint64_t, void* context, void*, std::uint64_t, std::uint32_t, std::uint32_t) {
    static_cast<CallbackGate*>(context)->OnCallback();
}

using ReadingCallback = void(STDMETHODCALLTYPE*)(std::uint64_t, void*, Reading*, bool);
using DeviceCallback = void(STDMETHODCALLTYPE*)(std::uint64_t, void*, void*, std::uint64_t, std::uint32_t, std::uint32_t);

std::uint64_t RegisterReading(void* context, ReadingCallback callback) {
    std::uint64_t token = 0;
    Expect(Call<HRESULT>(InputObject(), 8, static_cast<void*>(nullptr), kKindGamepad, 0.0f, context, callback, &token) == S_OK, "RegisterReadingCallback succeeds");
    return token;
}

bool Unregister(std::uint64_t token, std::uint64_t timeoutUs) {
    return Call<bool>(InputObject(), 13, token, timeoutUs);
}

std::thread ReleaseLater(CallbackGate& gate) {
    return std::thread([&gate] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        gate.Release();
    });
}

void UnregisterReadingCallbackWaitsForInFlight() {
    CallbackGate gate;
    const std::uint64_t token = RegisterReading(&gate, GatedReadingCallback);
    GamepadState pressed;
    pressed.buttons = kA;
    std::thread dispatch([pressed] { DispatchReadingCallbacks(pressed); });
    gate.WaitEntered();
    Expect(!Unregister(token, 0), "UnregisterCallback(0) fails while the callback runs");
    Expect(!Unregister(token, 15'000), "UnregisterCallback(15 ms) fails while the callback runs");
    Expect(!gate.finished.load(std::memory_order_acquire), "the callback still runs");
    std::thread releaser = ReleaseLater(gate);
    Expect(Unregister(token, 5'000'000), "UnregisterCallback waits for the callback");
    Expect(gate.finished.load(std::memory_order_acquire), "the callback finished before UnregisterCallback returned true");
    releaser.join();
    dispatch.join();
    const std::uint32_t before = gate.calls.load(std::memory_order_acquire);
    GamepadState other;
    other.buttons = kB;
    DispatchReadingCallbacks(other);
    Expect(gate.calls.load(std::memory_order_acquire) == before, "an unregistered callback is not called again");
}

void StopThenUnregisterStillWaits() {
    CallbackGate gate;
    const std::uint64_t token = RegisterReading(&gate, GatedReadingCallback);
    GamepadState pressed;
    pressed.buttons = kX;
    std::thread dispatch([pressed] { DispatchReadingCallbacks(pressed); });
    gate.WaitEntered();
    Call<void>(InputObject(), 12, token);
    Expect(!gate.finished.load(std::memory_order_acquire), "StopCallback does not wait");
    std::thread releaser = ReleaseLater(gate);
    Expect(Unregister(token, (std::numeric_limits<std::uint64_t>::max)()), "UnregisterCallback(UINT64_MAX) waits without overflow");
    Expect(gate.finished.load(std::memory_order_acquire), "the stopped callback finished before UnregisterCallback returned");
    releaser.join();
    dispatch.join();
}

struct SelfContext {
    std::atomic<std::uint32_t> calls{0};
    std::atomic<bool> unregistered{false};
};

void STDMETHODCALLTYPE SelfUnregisteringCallback(std::uint64_t token, void* context, Reading*, bool) {
    auto* self = static_cast<SelfContext*>(context);
    self->calls.fetch_add(1, std::memory_order_acq_rel);
    self->unregistered.store(Unregister(token, (std::numeric_limits<std::uint64_t>::max)()), std::memory_order_release);
}

void SelfUnregisterDoesNotDeadlock() {
    auto context = std::make_unique<SelfContext>();
    RegisterReading(context.get(), SelfUnregisteringCallback);
    GamepadState pressed;
    pressed.buttons = kY;
    DispatchReadingCallbacks(pressed);
    Expect(context->calls.load() == 1, "the self-unregistering callback ran once");
    Expect(context->unregistered.load(), "UnregisterCallback from inside the callback succeeds");
    pressed.buttons = kMenu;
    DispatchReadingCallbacks(pressed);
    Expect(context->calls.load() == 1, "the self-unregistered callback is not called again");
}

void UnregisterAsyncDeviceCallbackWaits() {
    CallbackGate gate;
    std::uint64_t token = 0;
    Expect(Call<HRESULT>(InputObject(), 9, static_cast<void*>(nullptr), kKindGamepad, kDeviceStatus, std::uint32_t{1}, static_cast<void*>(&gate),
                         static_cast<DeviceCallback>(GatedDeviceCallback), &token) == S_OK,
           "RegisterDeviceCallback(asynchronous) succeeds");
    gate.WaitEntered();
    Expect(!Unregister(token, 0), "UnregisterCallback(0) fails while the enumeration callback runs");
    Expect(!gate.finished.load(std::memory_order_acquire), "the enumeration callback still runs");
    std::thread releaser = ReleaseLater(gate);
    Expect(Unregister(token, 5'000'000), "UnregisterCallback waits for the enumeration thread");
    Expect(gate.finished.load(std::memory_order_acquire), "the enumeration callback finished first");
    releaser.join();
}

void BlockingDeviceCallbackRunsAtOnce() {
    std::atomic<std::uint32_t> calls{0};
    std::uint64_t token = 0;
    const auto callback = [](std::uint64_t, void* context, void* device, std::uint64_t, std::uint32_t status, std::uint32_t previous) {
        if (device == DeviceObject() && status == kDeviceStatus && previous == 0) static_cast<std::atomic<std::uint32_t>*>(context)->fetch_add(1);
    };
    Expect(Call<HRESULT>(InputObject(), 9, static_cast<void*>(nullptr), kKindGamepad, kDeviceStatus, kEnumerationBlocking, static_cast<void*>(&calls),
                         static_cast<DeviceCallback>(callback), &token) == S_OK &&
               calls.load() == 1,
           "a blocking enumeration reports the connected gamepad before returning");
}

void DeviceAnswers() {
    void* device = DeviceObject();
    const auto* info = Call<const DeviceInfo*>(device, 3);
    Expect(info != nullptr && info->infoSize == sizeof(DeviceInfo) && info->supportedInput == kKinds && info->deviceId == kDeviceId, "GetDeviceInfo describes the gamepad");
    Expect(Call<std::uint32_t>(device, 4) == kDeviceStatus, "GetDeviceStatus is connected");
    void* found = nullptr;
    Expect(Call<HRESULT>(InputObject(), 16, &kDeviceId, &found) == S_OK && found == device, "FindDeviceFromId finds the gamepad");
    std::array<std::uint8_t, 32> other{};
    found = device;
    Expect(Call<HRESULT>(InputObject(), 16, &other, &found) == kDeviceNotFound && found == nullptr, "FindDeviceFromId of another id");
}

void UnimplementedSlotsLogOnceAndFail() {
    Expect(Call<std::uint64_t>(DeviceObject(), 6) == kUnimplementedResult, "IGameInputDevice slot 6 returns E_NOTIMPL");
    Expect(Call<std::uint64_t>(DeviceObject(), 6) == kUnimplementedResult, "IGameInputDevice slot 6 returns E_NOTIMPL again");
    Expect(LogLinesContaining("IGameInputDevice::#6 first called") == 1, "an unimplemented slot is logged once");
    Expect(Call<std::uint64_t>(InputObject(), 14) == kUnimplementedResult, "IGameInput slot 14 returns E_NOTIMPL");
    Expect(LogLinesContaining("IGameInput::#14 first called") == 1, "IGameInput slot 14 is logged");
    Reading* reading = NewReading(GamepadState{});
    Expect(Call<std::uint64_t>(reading, 25) == kUnimplementedResult, "IGameInputReading slot 25 returns E_NOTIMPL");
    Expect(Call<std::uint32_t>(reading, 14, std::uint32_t{0}, static_cast<void*>(nullptr)) == 0, "GetKeyCount answers none");
    Expect(LogLinesContaining("IGameInputReading::GetKeyCount first called") == 1, "the first reading view call is logged");
    std::uint32_t navigation = 0;
    reading->state.buttons = kA | kDPadUp;
    Expect(Call<bool>(reading, 24, &navigation) && navigation == (0x4u | 0x10u), "GetUiNavigationState");
    Call<ULONG>(reading, 2);
}

void InjectorCommands() {
    Injector injector;
    const std::uint64_t t = 1'000'000;
    Expect(injector.Execute("press A 200", t) == "ok press A 200 ms", "press A 200");
    Expect(injector.Execute("PRESS down 150", t) == "ok press DPadDown 150 ms", "press accepts aliases in any case");
    Expect(injector.Execute("hold start", t) == "ok hold Menu", "hold START is Menu");
    Expect(injector.Execute("stick L 0.5 -1 300", t) == "ok stick L 0.5 -1 300 ms", "stick L");
    Expect(injector.Execute("trigger R 1 100", t) == "ok trigger R 1 100 ms", "trigger R");
    GamepadState state;
    state.rightTrigger = 0.25f;
    state.leftThumbstickX = 0.9f;
    injector.Apply(state, t + 50'000);
    Expect(state.buttons == (kA | kDPadDown | kMenu), "pressed and held buttons apply");
    Expect(state.leftThumbstickX == 0.5f && state.leftThumbstickY == -1.0f, "an injected stick replaces the stick");
    Expect(state.rightTrigger == 1.0f, "an injected trigger raises the trigger");
    GamepadState later;
    injector.Apply(later, t + 160'000);
    Expect(later.buttons == (kA | kMenu), "a press ends after its time");
    GamepadState after;
    injector.Apply(after, t + 400'000);
    Expect(after.buttons == kMenu && after.leftThumbstickX == 0.0f && after.rightTrigger == 0.0f, "only the hold remains");
    Expect(injector.Execute("release Menu", t) == "ok release Menu", "release");
    Expect(injector.Execute("press LeftThumbstick 50", t) == "ok press LeftThumbstick 50 ms", "full button names");
    Expect(injector.Execute("reset", t) == "ok reset", "reset");
    GamepadState cleared;
    injector.Apply(cleared, t + 1);
    Expect(cleared == GamepadState{}, "reset clears everything");
    Expect(injector.Execute("press Q 100", t).rfind("error unknown button", 0) == 0, "unknown button");
    Expect(injector.Execute("press A 0", t).rfind("error <ms>", 0) == 0, "zero ms");
    Expect(injector.Execute("press A 99999999", t).rfind("error <ms>", 0) == 0, "too many ms");
    Expect(injector.Execute("press A", t).rfind("error usage", 0) == 0, "missing ms");
    Expect(injector.Execute("stick L 2 0 100", t).rfind("error stick", 0) == 0, "stick out of range");
    Expect(injector.Execute("stick M 0 0 100", t).rfind("error stick side", 0) == 0, "stick side");
    Expect(injector.Execute("trigger L -0.5 100", t).rfind("error trigger", 0) == 0, "trigger out of range");
    Expect(injector.Execute("trigger L nan 100", t).rfind("error trigger", 0) == 0, "trigger nan");
    Expect(injector.Execute("wait 100", t).rfind("error unknown command 'wait'", 0) == 0, "wait is the client's");
}

void LineBufferSplitsLines() {
    LineBuffer buffer;
    std::vector<CommandLine> lines = buffer.Feed("press A 1");
    Expect(lines.empty(), "no line before a newline");
    lines = buffer.Feed("00\r\n\n  reset  \nhold");
    Expect(lines.size() == 2 && lines[0].text == "press A 100" && lines[1].text == "reset" && !lines[0].overflow, "lines are split, trimmed and blank ones skipped");
    lines = buffer.Feed(" B\n" + std::string(kMaxCommandLine + 10, 'x') + "\nreset\n");
    Expect(lines.size() == 3 && lines[0].text == "hold B" && lines[1].overflow && lines[2].text == "reset", "an overlong line is flagged and the next line survives");
}

bool ReadLine(HANDLE pipe, std::string& line) {
    line.clear();
    char c = 0;
    DWORD got = 0;
    while (ReadFile(pipe, &c, 1, &got, nullptr) != FALSE && got == 1) {
        if (c == '\n') return true;
        line.push_back(c);
    }
    return false;
}

void PipeRoundTrip() {
    const std::wstring name = L"\\\\.\\pipe\\anybox-gameinput-test-" + std::to_wstring(GetCurrentProcessId());
    Expect(StartPipeServer(name.c_str()), "the test pipe is served");
    Expect(!StartPipeServer(name.c_str()), "a second server of the same pipe is refused");
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 50 && pipe == INVALID_HANDLE_VALUE; ++attempt) {
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    Expect(pipe != INVALID_HANDLE_VALUE, "the client connects");
    if (pipe == INVALID_HANDLE_VALUE) return;
    const std::string commands = "press A 5000\nbogus\nhold X\n";
    DWORD written = 0;
    Expect(WriteFile(pipe, commands.data(), static_cast<DWORD>(commands.size()), &written, nullptr) != FALSE && written == commands.size(), "commands are sent");
    std::string reply;
    Expect(ReadLine(pipe, reply) && reply == "ok press A 5000 ms", "reply to press: " + reply);
    Expect(ReadLine(pipe, reply) && reply.rfind("error unknown command 'bogus'", 0) == 0, "reply to bogus: " + reply);
    Expect(ReadLine(pipe, reply) && reply == "ok hold X", "reply to hold: " + reply);
    Expect((Poll().buttons & (kA | kX)) == (kA | kX), "injected buttons reach the poll without focus");
    const std::string reset = "reset\n";
    WriteFile(pipe, reset.data(), static_cast<DWORD>(reset.size()), &written, nullptr);
    Expect(ReadLine(pipe, reply) && reply == "ok reset", "reply to reset: " + reply);
    Expect((Injected().Execute("reset", NowUs()) == "ok reset"), "reset again");
    GamepadState injectedOnly;
    Injected().Apply(injectedOnly, NowUs());
    Expect(injectedOnly == GamepadState{}, "reset through the pipe cleared the injection");
    CloseHandle(pipe);
}

}

int main() {
    SetLogSink(CaptureLog);
    XInputButtonsMapToGameInput();
    XInputPadsMerge();
    ReadingsComeAndGo();
    PressScriptParsesButtonsAndTimes();
    FocusGatesKeyboardButKeepsScriptedAndInjected();
    QueryInterfaceValidatesIidsAndRefcounts();
    UnregisterReadingCallbackWaitsForInFlight();
    StopThenUnregisterStillWaits();
    SelfUnregisterDoesNotDeadlock();
    UnregisterAsyncDeviceCallbackWaits();
    BlockingDeviceCallbackRunsAtOnce();
    DeviceAnswers();
    UnimplementedSlotsLogOnceAndFail();
    InjectorCommands();
    LineBufferSplitsLines();
    PipeRoundTrip();
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("gameinput: all checks passed\n");
    return 0;
}
