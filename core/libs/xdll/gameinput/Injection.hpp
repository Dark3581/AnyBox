#pragma once

#include "xdll/gameinput/Gamepad.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace abx::gameinput {

inline constexpr std::uint64_t kMaxInjectMilliseconds = 3'600'000;
inline constexpr std::size_t kMaxCommandLine = 256;

class Injector {
public:
    std::string Execute(std::string_view line, std::uint64_t nowUs);
    void Apply(GamepadState& state, std::uint64_t nowUs) const;

private:
    struct Stick {
        float x = 0.0f;
        float y = 0.0f;
        std::uint64_t untilUs = 0;
    };

    struct Trigger {
        float value = 0.0f;
        std::uint64_t untilUs = 0;
    };

    mutable std::mutex lock_;
    std::uint32_t held_ = 0;
    std::array<std::uint64_t, kButtonCount> pressedUntilUs_{};
    std::array<Stick, 2> sticks_{};
    std::array<Trigger, 2> triggers_{};
};

struct CommandLine {
    std::string text;
    bool overflow = false;
};

class LineBuffer {
public:
    std::vector<CommandLine> Feed(std::string_view bytes);

private:
    std::string pending_;
    bool overflow_ = false;
};

}
