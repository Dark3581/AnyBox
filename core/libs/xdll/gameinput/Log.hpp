#pragma once

namespace abx::gameinput {

using LogSink = void (*)(const char* line);

void SetLogSink(LogSink sink);
void Log(const char* format, ...);

}
