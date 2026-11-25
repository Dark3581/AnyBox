#include "xdll/gameinput/Gamepad.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <system_error>
#include <utility>

namespace abx::gameinput {

namespace {

constexpr std::array<std::pair<std::uint16_t, std::uint32_t>, 14> kXInputButtons{{
    {0x0001, kDPadUp},
    {0x0002, kDPadDown},
    {0x0004, kDPadLeft},
    {0x0008, kDPadRight},
    {0x0010, kMenu},
    {0x0020, kView},
    {0x0040, kLeftThumbstick},
    {0x0080, kRightThumbstick},
    {0x0100, kLeftShoulder},
    {0x0200, kRightShoulder},
    {0x1000, kA},
    {0x2000, kB},
    {0x4000, kX},
    {0x8000, kY},
}};

constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 12> kUiNavigationButtons{{
    {kMenu, 0x1},
    {kView, 0x2},
    {kA, 0x4},
    {kB, 0x8},
    {kDPadUp, 0x10},
    {kDPadDown, 0x20},
    {kDPadLeft, 0x40},
    {kDPadRight, 0x80},
    {kX, 0x100},
    {kY, 0x200},
    {kLeftShoulder, 0x4000},
    {kRightShoulder, 0x8000},
}};

struct NamedButton {
    std::string_view name;
    std::uint32_t button;
};

constexpr std::array<NamedButton, 22> kButtonNames{{
    {"A", kA},
    {"B", kB},
    {"X", kX},
    {"Y", kY},
    {"VIEW", kView},
    {"MENU", kMenu},
    {"DPADUP", kDPadUp},
    {"DPADDOWN", kDPadDown},
    {"DPADLEFT", kDPadLeft},
    {"DPADRIGHT", kDPadRight},
    {"LEFTSHOULDER", kLeftShoulder},
    {"RIGHTSHOULDER", kRightShoulder},
    {"LEFTTHUMBSTICK", kLeftThumbstick},
    {"RIGHTTHUMBSTICK", kRightThumbstick},
    {"UP", kDPadUp},
    {"DOWN", kDPadDown},
    {"LEFT", kDPadLeft},
    {"RIGHT", kDPadRight},
    {"START", kMenu},
    {"BACK", kView},
    {"LB", kLeftShoulder},
    {"RB", kRightShoulder},
}};

constexpr std::array<const char*, kButtonCount> kCanonicalNames{
    "Menu", "View", "A", "B", "X", "Y", "DPadUp", "DPadDown", "DPadLeft", "DPadRight", "LeftShoulder", "RightShoulder", "LeftThumbstick", "RightThumbstick",
};

std::optional<double> ParseSeconds(std::string_view text) {
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    if (text.empty()) return std::nullopt;
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) return std::nullopt;
    return value;
}

std::vector<std::string_view> Split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t at = text.find(separator, start);
        if (at == std::string_view::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, at - start));
        start = at + 1;
    }
}

}

std::string_view Trim(std::string_view text) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    return text;
}

std::string Upper(std::string_view text) {
    std::string upper(text);
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return upper;
}

std::uint32_t ButtonsFromXInput(std::uint16_t buttons) {
    std::uint32_t result = 0;
    for (const auto& [bit, button] : kXInputButtons) {
        if ((buttons & bit) != 0) result |= button;
    }
    return result;
}

std::uint32_t UiNavigation(std::uint32_t buttons) {
    std::uint32_t result = 0;
    for (const auto& [button, navigation] : kUiNavigationButtons) {
        if ((buttons & button) != 0) result |= navigation;
    }
    return result;
}

GamepadState GamepadFromXInput(std::span<const XInputPad> connected) {
    GamepadState state;
    if (connected.empty()) return state;
    const auto axis = [](std::int16_t value) { return (std::max)(static_cast<float>(value) / 32767.0f, -1.0f); };
    const XInputPad& first = connected.front();
    state.leftTrigger = static_cast<float>(first.leftTrigger) / 255.0f;
    state.rightTrigger = static_cast<float>(first.rightTrigger) / 255.0f;
    state.leftThumbstickX = axis(first.leftThumbX);
    state.leftThumbstickY = axis(first.leftThumbY);
    state.rightThumbstickX = axis(first.rightThumbX);
    state.rightThumbstickY = axis(first.rightThumbY);
    for (const XInputPad& pad : connected) state.buttons |= ButtonsFromXInput(pad.buttons);
    return state;
}

std::optional<std::uint32_t> ButtonNamed(std::string_view name) {
    const std::string upper = Upper(Trim(name));
    for (const NamedButton& entry : kButtonNames) {
        if (entry.name == upper) return entry.button;
    }
    return std::nullopt;
}

const char* ButtonName(std::uint32_t button) {
    for (std::size_t index = 0; index < kButtonCount; ++index) {
        if (button == (1u << index)) return kCanonicalNames[index];
    }
    return "?";
}

std::vector<Press> ParsePresses(std::string_view script, std::string_view aOnly, std::vector<std::string>& rejected) {
    std::vector<Press> presses;
    for (std::string_view item : Split(aOnly, ',')) {
        if (const std::optional<double> seconds = ParseSeconds(Trim(item))) presses.push_back({*seconds, kA});
    }
    for (std::string_view raw : Split(script, ',')) {
        const std::string_view item = Trim(raw);
        if (item.empty()) continue;
        const std::size_t colon = item.find(':');
        std::optional<double> seconds;
        std::optional<std::uint32_t> button;
        if (colon != std::string_view::npos) {
            seconds = ParseSeconds(Trim(item.substr(0, colon)));
            button = ButtonNamed(item.substr(colon + 1));
        }
        if (seconds && button) {
            presses.push_back({*seconds, *button});
        } else {
            rejected.emplace_back(item);
        }
    }
    return presses;
}

std::uint32_t ScriptedButtons(std::span<const Press> presses, double seconds) {
    std::uint32_t buttons = 0;
    for (const Press& press : presses) {
        if (seconds >= press.seconds && seconds < press.seconds + kPressSeconds) buttons |= press.buttons;
    }
    return buttons;
}

}
