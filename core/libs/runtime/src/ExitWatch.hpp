#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>

namespace abx {

struct ExitEvent {
    const char* how;
    const char* target;
    std::uint32_t code;
    bool hasCode;
    bool processEnds;
    bool stack;
    bool late;
    DWORD thread;
    void* caller;
};

using ExitReporter = void (*)(const ExitEvent& event);

void SetExitReporter(ExitReporter reporter);
void AddExitRedirects(const std::function<void(void* original, void* replacement)>& add);
const char* InstallTerminateHook();
void WatchMainThread(bool loadedAtStart);
DWORD MainThread();
bool ProcessExitReported();
void ReportShutdownUnseen();

}
