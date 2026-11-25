#pragma once

#include "xdll/gameinput/Gamepad.hpp"
#include "xdll/gameinput/Injection.hpp"

#include <cstdint>
#include <span>

namespace abx::gameinput {

inline constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\anybox-gameinput";

std::uint64_t NowUs();
double SecondsSinceProcessStart();

GamepadState Poll();
Injector& Injected();
std::span<const Press> ScriptedPresses();
void SetRumble(float low, float high);

bool StartPipeServer(const wchar_t* name);
void StartSources();

}
