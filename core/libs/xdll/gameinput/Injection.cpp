#include "xdll/gameinput/Injection.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <optional>
#include <system_error>

namespace abx::gameinput {

namespace {

std::vector<std::string_view> Tokens(std::string_view line) {
    std::vector<std::string_view> tokens;
    std::size_t at = 0;
    while (at < line.size()) {
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
        const std::size_t start = at;
        while (at < line.size() && line[at] != ' ' && line[at] != '\t') ++at;
        if (at > start) tokens.push_back(line.substr(start, at - start));
    }
    return tokens;
}

std::optional<std::uint64_t> Milliseconds(std::string_view text) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || value == 0 || value > kMaxInjectMilliseconds) return std::nullopt;
    return value;
}

std::optional<float> Number(std::string_view text, float low, float high) {
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    if (text.empty()) return std::nullopt;
    double value = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || !std::isfinite(value) || value < low || value > high) return std::nullopt;
    return static_cast<float>(value);
}

std::optional<std::size_t> Side(std::string_view text) {
    const std::string upper = Upper(text);
    if (upper == "L") return 0;
    if (upper == "R") return 1;
    return std::nullopt;
}

std::size_t ButtonIndex(std::uint32_t button) {
    std::size_t index = 0;
    while (index < kButtonCount && button != (1u << index)) ++index;
    return index;
}

std::string Format(const char* format, auto... values) {
    char buffer[kMaxCommandLine + 64];
    std::snprintf(buffer, sizeof(buffer), format, values...);
    return buffer;
}

std::string MillisecondsError() {
    return Format("error <ms> must be a whole number from 1 to %llu", static_cast<unsigned long long>(kMaxInjectMilliseconds));
}

}

std::string Injector::Execute(std::string_view line, std::uint64_t nowUs) {
    const std::vector<std::string_view> tokens = Tokens(line);
    if (tokens.empty()) return "error empty command";
    const std::string command = Upper(tokens[0]);
    const auto button = [&](std::string_view name) { return ButtonNamed(name); };
    const std::lock_guard guard(lock_);
    if (command == "PRESS") {
        if (tokens.size() != 3) return "error usage: press <BUTTON> <ms>";
        const std::optional<std::uint32_t> pressed = button(tokens[1]);
        if (!pressed) return "error unknown button '" + std::string(tokens[1]) + "'";
        const std::optional<std::uint64_t> ms = Milliseconds(tokens[2]);
        if (!ms) return MillisecondsError();
        std::uint64_t& until = pressedUntilUs_[ButtonIndex(*pressed)];
        until = (std::max)(until, nowUs + *ms * 1000);
        return Format("ok press %s %llu ms", ButtonName(*pressed), static_cast<unsigned long long>(*ms));
    }
    if (command == "HOLD" || command == "RELEASE") {
        if (tokens.size() != 2) return command == "HOLD" ? "error usage: hold <BUTTON>" : "error usage: release <BUTTON>";
        const std::optional<std::uint32_t> named = button(tokens[1]);
        if (!named) return "error unknown button '" + std::string(tokens[1]) + "'";
        if (command == "HOLD") {
            held_ |= *named;
            return Format("ok hold %s", ButtonName(*named));
        }
        held_ &= ~*named;
        pressedUntilUs_[ButtonIndex(*named)] = 0;
        return Format("ok release %s", ButtonName(*named));
    }
    if (command == "STICK") {
        if (tokens.size() != 5) return "error usage: stick L|R <x> <y> <ms>";
        const std::optional<std::size_t> side = Side(tokens[1]);
        if (!side) return "error stick side must be L or R";
        const std::optional<float> x = Number(tokens[2], -1.0f, 1.0f);
        const std::optional<float> y = Number(tokens[3], -1.0f, 1.0f);
        if (!x || !y) return "error stick <x> and <y> must be numbers from -1 to 1";
        const std::optional<std::uint64_t> ms = Milliseconds(tokens[4]);
        if (!ms) return MillisecondsError();
        sticks_[*side] = {*x, *y, nowUs + *ms * 1000};
        return Format("ok stick %s %g %g %llu ms", *side == 0 ? "L" : "R", static_cast<double>(*x), static_cast<double>(*y), static_cast<unsigned long long>(*ms));
    }
    if (command == "TRIGGER") {
        if (tokens.size() != 4) return "error usage: trigger L|R <v> <ms>";
        const std::optional<std::size_t> side = Side(tokens[1]);
        if (!side) return "error trigger side must be L or R";
        const std::optional<float> value = Number(tokens[2], 0.0f, 1.0f);
        if (!value) return "error trigger <v> must be a number from 0 to 1";
        const std::optional<std::uint64_t> ms = Milliseconds(tokens[3]);
        if (!ms) return MillisecondsError();
        triggers_[*side] = {*value, nowUs + *ms * 1000};
        return Format("ok trigger %s %g %llu ms", *side == 0 ? "L" : "R", static_cast<double>(*value), static_cast<unsigned long long>(*ms));
    }
    if (command == "RESET") {
        if (tokens.size() != 1) return "error usage: reset";
        held_ = 0;
        pressedUntilUs_ = {};
        sticks_ = {};
        triggers_ = {};
        return "ok reset";
    }
    return "error unknown command '" + std::string(tokens[0]) + "' (press, hold, release, stick, trigger, reset)";
}

void Injector::Apply(GamepadState& state, std::uint64_t nowUs) const {
    const std::lock_guard guard(lock_);
    std::uint32_t buttons = held_;
    for (std::size_t index = 0; index < kButtonCount; ++index) {
        if (pressedUntilUs_[index] > nowUs) buttons |= 1u << index;
    }
    state.buttons |= buttons;
    if (sticks_[0].untilUs > nowUs) {
        state.leftThumbstickX = sticks_[0].x;
        state.leftThumbstickY = sticks_[0].y;
    }
    if (sticks_[1].untilUs > nowUs) {
        state.rightThumbstickX = sticks_[1].x;
        state.rightThumbstickY = sticks_[1].y;
    }
    if (triggers_[0].untilUs > nowUs) state.leftTrigger = (std::max)(state.leftTrigger, triggers_[0].value);
    if (triggers_[1].untilUs > nowUs) state.rightTrigger = (std::max)(state.rightTrigger, triggers_[1].value);
}

std::vector<CommandLine> LineBuffer::Feed(std::string_view bytes) {
    std::vector<CommandLine> lines;
    for (const char c : bytes) {
        if (c != '\n') {
            if (pending_.size() < kMaxCommandLine) {
                pending_.push_back(c);
            } else {
                overflow_ = true;
            }
            continue;
        }
        const std::string_view text = Trim(pending_);
        if (overflow_) {
            lines.push_back({std::string(text.substr(0, 32)), true});
        } else if (!text.empty()) {
            lines.push_back({std::string(text), false});
        }
        pending_.clear();
        overflow_ = false;
    }
    return lines;
}

}
