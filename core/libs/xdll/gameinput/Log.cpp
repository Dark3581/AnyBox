#include "xdll/gameinput/Log.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace abx::gameinput {

namespace {

std::atomic<LogSink> g_sink{nullptr};

}

void SetLogSink(LogSink sink) {
    g_sink.store(sink);
}

void Log(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    const LogSink sink = g_sink.load();
    if (sink != nullptr) {
        sink(buffer);
        return;
    }
    std::fprintf(stderr, "[gameinput] %s\n", buffer);
}

}
