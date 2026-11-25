#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace abx::gameinput {

struct GamepadState {
    std::uint32_t buttons = 0;
    float leftTrigger = 0.0f;
    float rightTrigger = 0.0f;
    float leftThumbstickX = 0.0f;
    float leftThumbstickY = 0.0f;
    float rightThumbstickX = 0.0f;
    float rightThumbstickY = 0.0f;

    bool operator==(const GamepadState&) const = default;
};

static_assert(sizeof(GamepadState) == 28);

inline constexpr std::uint32_t kMenu = 0x1;
inline constexpr std::uint32_t kView = 0x2;
inline constexpr std::uint32_t kA = 0x4;
inline constexpr std::uint32_t kB = 0x8;
inline constexpr std::uint32_t kX = 0x10;
inline constexpr std::uint32_t kY = 0x20;
inline constexpr std::uint32_t kDPadUp = 0x40;
inline constexpr std::uint32_t kDPadDown = 0x80;
inline constexpr std::uint32_t kDPadLeft = 0x100;
inline constexpr std::uint32_t kDPadRight = 0x200;
inline constexpr std::uint32_t kLeftShoulder = 0x400;
inline constexpr std::uint32_t kRightShoulder = 0x800;
inline constexpr std::uint32_t kLeftThumbstick = 0x1000;
inline constexpr std::uint32_t kRightThumbstick = 0x2000;
inline constexpr std::size_t kButtonCount = 14;

inline constexpr std::uint32_t kKindController = 0xE;
inline constexpr std::uint32_t kKindGamepad = 0x4'0000;
inline constexpr std::uint32_t kKindUiNavigation = 0x100'0000;
inline constexpr std::uint32_t kKinds = kKindController | kKindGamepad | kKindUiNavigation;
inline constexpr std::uint32_t kDeviceStatus = 0x1 | 0x2 | 0x4 | 0x40;
inline constexpr std::uint32_t kEnumerationBlocking = 2;

struct XInputPad {
    std::uint16_t buttons = 0;
    std::uint8_t leftTrigger = 0;
    std::uint8_t rightTrigger = 0;
    std::int16_t leftThumbX = 0;
    std::int16_t leftThumbY = 0;
    std::int16_t rightThumbX = 0;
    std::int16_t rightThumbY = 0;
};

static_assert(sizeof(XInputPad) == 12);

std::uint32_t ButtonsFromXInput(std::uint16_t buttons);
std::uint32_t UiNavigation(std::uint32_t buttons);
GamepadState GamepadFromXInput(std::span<const XInputPad> connected);

std::optional<std::uint32_t> ButtonNamed(std::string_view name);
const char* ButtonName(std::uint32_t button);

struct Press {
    double seconds = 0.0;
    std::uint32_t buttons = 0;

    bool operator==(const Press&) const = default;
};

inline constexpr double kPressSeconds = 0.2;

std::vector<Press> ParsePresses(std::string_view script, std::string_view aOnly, std::vector<std::string>& rejected);
std::uint32_t ScriptedButtons(std::span<const Press> presses, double seconds);

template <class Keyboard, class Scripted, class Injected>
void MergeHostInputs(GamepadState& state, bool focused, Keyboard&& readKeyboard, Scripted&& readScripted, Injected&& applyInjected) {
    if (focused) {
        state.buttons |= readKeyboard();
    }
    state.buttons |= readScripted();
    applyInjected(state);
}

std::string_view Trim(std::string_view text);
std::string Upper(std::string_view text);

}
