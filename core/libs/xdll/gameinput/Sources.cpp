#include "xdll/gameinput/Sources.hpp"

#include "xdll/gameinput/Log.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace abx::gameinput {

namespace {

struct XInputState {
    std::uint32_t packet = 0;
    XInputPad pad;
};

static_assert(sizeof(XInputState) == 16);

struct XInputVibration {
    std::uint16_t left = 0;
    std::uint16_t right = 0;
};

using GetStateFunction = DWORD(WINAPI*)(DWORD, XInputState*);
using SetStateFunction = DWORD(WINAPI*)(DWORD, XInputVibration*);

struct XInputApi {
    GetStateFunction getState = nullptr;
    SetStateFunction setState = nullptr;
    const char* library = nullptr;
};

const XInputApi& XInput() {
    static const XInputApi api = [] {
        XInputApi loaded;
        for (const char* name : {"xinput1_4.dll", "xinput9_1_0.dll"}) {
            const HMODULE module = LoadLibraryA(name);
            if (module == nullptr) continue;
            const auto get = reinterpret_cast<GetStateFunction>(reinterpret_cast<void*>(GetProcAddress(module, "XInputGetState")));
            const auto set = reinterpret_cast<SetStateFunction>(reinterpret_cast<void*>(GetProcAddress(module, "XInputSetState")));
            if (get != nullptr && set != nullptr) {
                loaded = {get, set, name};
                return loaded;
            }
            FreeLibrary(module);
        }
        Log("neither xinput1_4.dll nor xinput9_1_0.dll could be loaded: the PC's controllers are not read");
        return loaded;
    }();
    return api;
}

constexpr std::uint32_t kXInputSlots = 4;
constexpr std::uint64_t kSearchUs = 1'000'000;

struct XInputSlot {
    std::atomic<bool> connected{false};
    std::atomic<std::uint64_t> lastSearchUs{0};
};

std::array<XInputSlot, kXInputSlots> g_slots;
std::atomic<std::uint32_t> g_rumbleSlot{0};

GamepadState PollXInput() {
    const XInputApi& api = XInput();
    if (api.getState == nullptr) return {};
    std::array<XInputPad, kXInputSlots> pads{};
    std::size_t count = 0;
    std::uint32_t first = kXInputSlots;
    for (std::uint32_t index = 0; index < kXInputSlots; ++index) {
        XInputSlot& slot = g_slots[index];
        if (!slot.connected.load(std::memory_order_relaxed)) {
            const std::uint64_t now = NowUs();
            const std::uint64_t last = slot.lastSearchUs.load(std::memory_order_relaxed);
            if (last != 0 && now >= last && now - last < kSearchUs) continue;
            slot.lastSearchUs.store(now, std::memory_order_relaxed);
        }
        XInputState state{};
        const DWORD result = api.getState(index, &state);
        if (result != ERROR_SUCCESS && result != ERROR_DEVICE_NOT_CONNECTED) {
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true)) Log("XInputGetState(%u) returned %lu", index, static_cast<unsigned long>(result));
        }
        if (result == ERROR_SUCCESS) {
            if (!slot.connected.exchange(true)) {
                Log("XInput slot %u connected: its buttons reach the title's gamepad (sticks and triggers come from the lowest connected slot)", index);
            }
            if (first == kXInputSlots) first = index;
            pads[count++] = state.pad;
        } else if (slot.connected.exchange(false)) {
            Log("XInput slot %u disconnected", index);
            slot.lastSearchUs.store(NowUs(), std::memory_order_relaxed);
        }
    }
    if (first != kXInputSlots) g_rumbleSlot.store(first, std::memory_order_relaxed);
    return GamepadFromXInput(std::span<const XInputPad>(pads.data(), count));
}

bool ProcessHasFocus() {
    const HWND window = GetForegroundWindow();
    if (window == nullptr) return false;
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    return process == GetCurrentProcessId();
}

std::uint32_t KeyboardButtons() {
    constexpr std::array<std::pair<int, std::uint32_t>, 10> kKeys{{
        {VK_RETURN, kA},
        {VK_SPACE, kA},
        {VK_ESCAPE, kB},
        {VK_BACK, kB},
        {VK_UP, kDPadUp},
        {VK_DOWN, kDPadDown},
        {VK_LEFT, kDPadLeft},
        {VK_RIGHT, kDPadRight},
        {VK_TAB, kView},
        {'P', kMenu},
    }};
    std::uint32_t buttons = 0;
    for (const auto& [key, button] : kKeys) {
        if ((static_cast<std::uint16_t>(GetAsyncKeyState(key)) & 0x8000) != 0) buttons |= button;
    }
    return buttons;
}

std::string Environment(const char* name) {
    char buffer[4096];
    const DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    if (length == 0 || length >= sizeof(buffer)) return {};
    return std::string(buffer, length);
}

std::uint64_t FileTimeValue(const FILETIME& time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

std::uint64_t SystemTimeNow() {
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    return FileTimeValue(now);
}

std::string Narrow(const wchar_t* text) {
    std::string narrow;
    for (; *text != L'\0'; ++text) narrow.push_back(*text < 0x80 ? static_cast<char>(*text) : '?');
    return narrow;
}

struct HandleCloser {
    void operator()(void* handle) const {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

bool WriteReply(HANDLE pipe, const std::string& reply) {
    const std::string line = reply + "\n";
    DWORD written = 0;
    return WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) != FALSE && written == line.size();
}

void ServeClient(HANDLE pipe) {
    LineBuffer buffer;
    std::array<char, 512> chunk{};
    for (;;) {
        DWORD got = 0;
        if (ReadFile(pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr) == FALSE) return;
        for (const CommandLine& line : buffer.Feed(std::string_view(chunk.data(), got))) {
            std::string reply;
            if (line.overflow) {
                reply = "error line longer than " + std::to_string(kMaxCommandLine) + " characters";
            } else {
                reply = Injected().Execute(line.text, NowUs());
            }
            Log("pipe command '%s': %s", line.text.c_str(), reply.c_str());
            if (!WriteReply(pipe, reply)) return;
        }
    }
}

void ServePipe(HANDLE pipe) {
    bool reported = false;
    for (;;) {
        const bool connected = ConnectNamedPipe(pipe, nullptr) != FALSE || GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected) {
            if (!reported) {
                reported = true;
                Log("pipe: ConnectNamedPipe failed with %lu; retrying", static_cast<unsigned long>(GetLastError()));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        ServeClient(pipe);
        DisconnectNamedPipe(pipe);
    }
}

}

std::uint64_t NowUs() {
    static const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()) + 1;
}

double SecondsSinceProcessStart() {
    static const std::uint64_t created = [] {
        FILETIME creation{};
        FILETIME exit{};
        FILETIME kernel{};
        FILETIME user{};
        if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) != FALSE) return FileTimeValue(creation);
        return SystemTimeNow();
    }();
    const std::uint64_t now = SystemTimeNow();
    return now > created ? static_cast<double>(now - created) / 1e7 : 0.0;
}

Injector& Injected() {
    static Injector injector;
    return injector;
}

std::span<const Press> ScriptedPresses() {
    static const std::vector<Press> presses = [] {
        std::vector<std::string> rejected;
        std::vector<Press> parsed = ParsePresses(Environment("ANYBOX_PRESS_AT"), Environment("ANYBOX_PRESS_A_AT"), rejected);
        for (const std::string& item : rejected) Log("ANYBOX_PRESS_AT entry '%s' not understood (seconds:BUTTON)", item.c_str());
        return parsed;
    }();
    return presses;
}

GamepadState Poll() {
    GamepadState state = PollXInput();
    MergeHostInputs(
        state, ProcessHasFocus(), [] { return KeyboardButtons(); }, [] { return ScriptedButtons(ScriptedPresses(), SecondsSinceProcessStart()); },
        [](GamepadState& merged) { Injected().Apply(merged, NowUs()); });
    static std::atomic<std::uint32_t> seen{0};
    const std::uint32_t fresh = state.buttons & ~seen.fetch_or(state.buttons, std::memory_order_relaxed);
    if (fresh != 0) Log("gamepad buttons 0x%x pressed for the first time", fresh);
    return state;
}

void SetRumble(float low, float high) {
    const XInputApi& api = XInput();
    if (api.setState == nullptr) return;
    const auto speed = [](float value) { return static_cast<std::uint16_t>(std::clamp(value, 0.0f, 1.0f) * 65535.0f); };
    XInputVibration vibration{speed(low), speed(high)};
    api.setState(g_rumbleSlot.load(std::memory_order_relaxed), &vibration);
}

bool StartPipeServer(const wchar_t* name) {
    const HANDLE created = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                            4096, 4096, 0, nullptr);
    if (created == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        Log("pipe %s not served: CreateNamedPipe failed with %lu%s", Narrow(name).c_str(), static_cast<unsigned long>(error),
            error == ERROR_ACCESS_DENIED || error == ERROR_PIPE_BUSY ? " (another server owns the pipe)" : "");
        return false;
    }
    UniqueHandle pipe(created);
    try {
        std::thread([served = std::move(pipe)] { ServePipe(served.get()); }).detach();
    } catch (const std::system_error& error) {
        Log("pipe %s not served: its thread did not start (%s)", Narrow(name).c_str(), error.what());
        return false;
    }
    return true;
}

void StartSources() {
    const XInputApi& api = XInput();
    const bool served = StartPipeServer(kPipeName);
    Log("one gamepad, merged from: XInput pads 0-3 %s%s (buttons of every connected pad, sticks and triggers of the lowest connected slot); "
        "the keyboard while a window of this process has focus (Enter/Space A, Escape/Backspace B, arrows D-pad, Tab View, P Menu); "
        "the pipe %s%s (any focus); %zu scripted press(es) from ANYBOX_PRESS_A_AT / ANYBOX_PRESS_AT",
        api.library != nullptr ? "through " : "", api.library != nullptr ? api.library : "unavailable (no XInput DLL)", Narrow(kPipeName).c_str(),
        served ? "" : " (not served)", ScriptedPresses().size());
}

}
