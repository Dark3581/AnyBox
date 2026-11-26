#pragma once

#include <cstddef>
#include <cstdint>

#if defined(ABX_RUNTIME_BUILD)
#define ABX_RUNTIME_API extern "C" __declspec(dllexport)
#else
#define ABX_RUNTIME_API extern "C" __declspec(dllimport)
#endif

#define ABX_API extern "C" __declspec(dllexport)

#ifndef ABX_LIBRARY_NAME
#define ABX_LIBRARY_NAME "unknown"
#endif

inline constexpr int AbxExitNotImplemented = 0xAB01;
inline constexpr int AbxExitUnsupported = 0xAB02;

ABX_RUNTIME_API void AbxNotImplementedAt [[noreturn]] (const char* library, const char* function, const char* file, int line);
ABX_RUNTIME_API void AbxUnsupportedAt [[noreturn]] (const char* library, const char* function, const char* message, const char* file, int line);
ABX_RUNTIME_API void AbxLog(const char* channel, const char* format, ...);
ABX_RUNTIME_API bool AbxTraceEnabled(const char* library);
ABX_RUNTIME_API const char* AbxTitleId();
ABX_RUNTIME_API const char* AbxTitleName();
ABX_RUNTIME_API const char* AbxTitleDeviceFamily();
ABX_RUNTIME_API const char* AbxSetting(const char* key);
ABX_RUNTIME_API std::size_t AbxMapConsolePath(const wchar_t* path, bool write, wchar_t* buffer, std::size_t capacity);

using AbxRefusedBuffer = bool (*)(void* context, std::uint64_t address, std::uint64_t bytes, bool write, std::uint64_t caller);

ABX_RUNTIME_API bool AbxWatchRefusedBuffers(AbxRefusedBuffer handler, void* context);

#define AbxNotImplemented(name) AbxNotImplementedAt(ABX_LIBRARY_NAME, name, __FILE__, __LINE__)
#define AbxUnsupported(name, message) AbxUnsupportedAt(ABX_LIBRARY_NAME, name, message, __FILE__, __LINE__)
#define AbxTrace(...)                                                      \
    do {                                                                   \
        static const bool abxTraceOn = AbxTraceEnabled(ABX_LIBRARY_NAME);  \
        if (abxTraceOn) {                                                  \
            AbxLog(ABX_LIBRARY_NAME, __VA_ARGS__);                         \
        }                                                                  \
    } while (false)
