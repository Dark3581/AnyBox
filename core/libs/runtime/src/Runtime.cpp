#include "CpuFixups.hpp"
#include "Probe.hpp"
#include "AbxRuntime.hpp"
#include "LoaderRedirect.hpp"
#include "Settings.hpp"
#include "TitleAsserts.hpp"
#include "TitleConfig.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

#ifndef ANYBOX_COMMIT
#define ANYBOX_COMMIT ""
#endif
#ifndef ANYBOX_RELEASE
#define ANYBOX_RELEASE ""
#endif
#ifndef ANYBOX_MODIFIED
#define ANYBOX_MODIFIED -1
#endif

namespace {

constexpr std::size_t MaxReportBytes = 65536;
constexpr std::size_t MaxLibraries = 128;
constexpr std::size_t MaxFrames = 32;

std::array<std::atomic<const char*>, MaxLibraries> g_libraries{};
std::atomic<std::size_t> g_libraryCount{0};
std::atomic<bool> g_reportWritten{false};
std::chrono::steady_clock::time_point g_start;
std::mutex g_logMutex;
FILE* g_logFile = nullptr;
std::once_flag g_titleOnce;
abx::TitleConfig g_title;
std::once_flag g_settingsOnce;
abx::Settings g_settings;
std::mutex g_settingsMutex;
std::set<std::string> g_settingsLogged;

std::wstring ExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return L".";
        }
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::string Environment(const char* name) {
    char buffer[1024];
    const DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    if (length == 0 || length >= sizeof(buffer)) {
        return {};
    }
    return std::string(buffer, length);
}

std::vector<std::string> TraceNames() {
    std::vector<std::string> names;
    std::string name;
    for (const char c : Environment("ANYBOX_TRACE") + ",") {
        if (c == ',') {
            if (!name.empty()) {
                names.push_back(name);
            }
            name.clear();
        } else if (c != ' ' && c != '\t') {
            name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return names;
}

const abx::TitleConfig& Title() {
    std::call_once(g_titleOnce, [] { g_title = abx::LoadTitleConfig(ExecutableDirectory() + L"\\MicrosoftGame.config"); });
    return g_title;
}

std::string Json(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

std::string JsonOrNull(const char* text) {
    return text != nullptr && *text != '\0' ? Json(text) : "null";
}

std::string JsonOrNull(const std::string& text) {
    return text.empty() ? "null" : Json(text);
}

std::string CpuName() {
    std::array<unsigned int, 12> brand{};
#if defined(_MSC_VER)
    int registers[4];
    __cpuid(registers, 0x80000000);
    if (static_cast<unsigned int>(registers[0]) < 0x80000004) {
        return {};
    }
    for (int i = 0; i < 3; ++i) {
        __cpuid(registers, 0x80000002 + i);
        std::memcpy(&brand[i * 4], registers, sizeof(registers));
    }
#else
    if (__get_cpuid_max(0x80000000, nullptr) < 0x80000004) {
        return {};
    }
    for (unsigned int i = 0; i < 3; ++i) {
        __get_cpuid(0x80000002 + i, &brand[i * 4], &brand[i * 4 + 1], &brand[i * 4 + 2], &brand[i * 4 + 3]);
    }
#endif
    std::string name(reinterpret_cast<const char*>(brand.data()), brand.size() * sizeof(unsigned int));
    name.resize(std::strlen(name.c_str()));
    const auto first = name.find_first_not_of(' ');
    const auto last = name.find_last_not_of(' ');
    return first == std::string::npos ? std::string{} : name.substr(first, last - first + 1);
}

std::string OsVersion() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return {};
    }
    const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    if (rtlGetVersion == nullptr) {
        return {};
    }
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtlGetVersion(&info) != 0) {
        return {};
    }
    return std::format("{}.{}.{}", info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
}

std::string RamGiB() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return "null";
    }
    const unsigned long long gib = (status.ullTotalPhys + (1ull << 29)) >> 30;
    return std::to_string(gib == 0 ? 1 : gib);
}

std::string UtcNow() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_s(&utc, &now);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

std::string ModuleOf(std::uint64_t address, std::uint64_t& offset) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(static_cast<std::uintptr_t>(address)), &module)) {
        return {};
    }
    offset = address - reinterpret_cast<std::uintptr_t>(module);
    char path[MAX_PATH];
    const DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
    if (length == 0) {
        return {};
    }
    const std::string full(path, length);
    const auto slash = full.find_last_of("\\/");
    return slash == std::string::npos ? full : full.substr(slash + 1);
}

std::string FrameText(std::uint64_t address, std::string* module = nullptr) {
    std::uint64_t offset = 0;
    const std::string name = ModuleOf(address, offset);
    if (module != nullptr) *module = name;
    return name.empty() ? std::format("0x{:x}", address) : std::format("{}+0x{:x}", name, offset);
}

bool IsAnyBoxModule(const std::string& module) {
    return module.size() > 3 && std::tolower(static_cast<unsigned char>(module[0])) == 'a' && std::tolower(static_cast<unsigned char>(module[1])) == 'b' && module[2] == '_';
}

std::string FrameJson(std::uint64_t address) {
    std::uint64_t offset = 0;
    const std::string module = ModuleOf(address, offset);
    return std::format("{{\"module\": {}, \"offset\": {}, \"address\": \"0x{:x}\"}}", JsonOrNull(module),
        module.empty() ? "null" : std::format("\"0x{:x}\"", offset), address);
}

bool IsSystemModule(std::uint64_t address) {
    static const std::wstring windows = [] {
        wchar_t path[MAX_PATH];
        const UINT length = GetSystemWindowsDirectoryW(path, MAX_PATH);
        std::wstring text = length != 0 && length < MAX_PATH ? std::wstring(path, length) : std::wstring{};
        if (!text.empty() && text.back() != L'\\') text += L'\\';
        return text;
    }();
    HMODULE module = nullptr;
    if (windows.empty() || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(static_cast<std::uintptr_t>(address)), &module)) {
        return false;
    }
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    return length >= windows.size() && CompareStringOrdinal(path, static_cast<int>(windows.size()), windows.c_str(), static_cast<int>(windows.size()), TRUE) == CSTR_EQUAL;
}

std::size_t CallerFrame(const std::uint64_t* frames, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        std::uint64_t offset = 0;
        const std::string module = ModuleOf(frames[index], offset);
        if (!module.empty() && !IsAnyBoxModule(module) && !IsSystemModule(frames[index])) return index;
    }
    return count;
}

std::size_t CaptureFrames(std::uint64_t* frames) {
    void* raw[MaxFrames]{};
    const USHORT count = RtlCaptureStackBackTrace(2, static_cast<DWORD>(MaxFrames), raw, nullptr);
    for (USHORT index = 0; index < count; ++index) frames[index] = reinterpret_cast<std::uintptr_t>(raw[index]);
    return count;
}

std::size_t UnwindFramesWithin(const CONTEXT& start, std::uint64_t* frames, ULONG_PTR low, ULONG_PTR high) {
    CONTEXT context = start;
    std::size_t count = 0;
    while (count < MaxFrames && context.Rip != 0) {
        frames[count++] = context.Rip;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (function != nullptr) {
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
        } else {
            if (context.Rsp < low || context.Rsp + sizeof(DWORD64) > high) break;
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += sizeof(DWORD64);
        }
        if (context.Rsp < low || context.Rsp > high) break;
    }
    return count;
}

std::size_t UnwindFrames(const CONTEXT& start, std::uint64_t* frames) {
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    return UnwindFramesWithin(start, frames, reinterpret_cast<ULONG_PTR>(tib->StackLimit), reinterpret_cast<ULONG_PTR>(tib->StackBase));
}

void Emit(const std::string& line);

constexpr std::size_t MaxStackHits = 12;
constexpr std::size_t MaxScannedStackBytes = 256 * 1024;

struct ThreadFrames {
    DWORD id = 0;
    std::size_t count = 0;
    std::uint64_t frames[MaxFrames]{};
    std::size_t hits = 0;
    std::uint64_t stackHits[MaxStackHits]{};
};

constexpr std::size_t MaxDumpedThreads = 256;
constexpr std::size_t DumpedFramesPerThread = MaxFrames;

std::vector<std::pair<std::uint64_t, std::uint64_t>> AnyBoxModuleRanges() {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return ranges;
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    for (BOOL more = Module32FirstW(snapshot, &module); more; more = Module32NextW(snapshot, &module)) {
        if (_wcsnicmp(module.szModule, L"ab_", 3) == 0) {
            const auto base = reinterpret_cast<std::uint64_t>(module.modBaseAddr);
            ranges.emplace_back(base, base + module.modBaseSize);
        }
    }
    CloseHandle(snapshot);
    return ranges;
}

void EmitOtherThreads(const char* when = "at the crash") {
    std::vector<DWORD> ids;
    ids.reserve(MaxDumpedThreads);
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD process = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    for (BOOL more = Thread32First(snapshot, &entry); more && ids.size() < MaxDumpedThreads; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self) ids.push_back(entry.th32ThreadID);
    }
    CloseHandle(snapshot);
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> anybox = AnyBoxModuleRanges();
    std::vector<ThreadFrames> dumped(ids.size());
    std::size_t filled = 0;
    for (const DWORD id : ids) {
        const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
        if (thread == nullptr) continue;
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context)) {
                MEMORY_BASIC_INFORMATION stack{};
                if (VirtualQuery(reinterpret_cast<const void*>(context.Rsp), &stack, sizeof(stack)) == sizeof(stack)) {
                    const auto low = reinterpret_cast<ULONG_PTR>(stack.AllocationBase);
                    const auto high = reinterpret_cast<ULONG_PTR>(stack.BaseAddress) + stack.RegionSize;
                    ThreadFrames& target = dumped[filled++];
                    target.id = id;
                    target.count = UnwindFramesWithin(context, target.frames, low, high);
                    const ULONG_PTR scanEnd = (std::min)(high, static_cast<ULONG_PTR>(context.Rsp + MaxScannedStackBytes));
                    for (ULONG_PTR at = context.Rsp & ~static_cast<ULONG_PTR>(7); at + sizeof(std::uint64_t) <= scanEnd && target.hits < MaxStackHits; at += sizeof(std::uint64_t)) {
                        const std::uint64_t word = *reinterpret_cast<const std::uint64_t*>(at);
                        for (const auto& [begin, end] : anybox) {
                            if (word >= begin && word < end) {
                                target.stackHits[target.hits++] = word;
                                break;
                            }
                        }
                    }
                }
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    Emit(std::format("[anybox] other threads {} ({} of {}):\n", when, filled, ids.size()));
    for (std::size_t index = 0; index < filled; ++index) {
        const ThreadFrames& thread = dumped[index];
        Emit(std::format("[anybox]  thread {}:\n", thread.id));
        for (std::size_t frame = 0; frame < (std::min)(thread.count, DumpedFramesPerThread); ++frame) {
            Emit(std::format("[anybox]     #{:02} {}\n", frame, FrameText(thread.frames[frame])));
        }
        for (std::size_t hit = 0; hit < thread.hits; ++hit) {
            Emit(std::format("[anybox]     stack word -> {}\n", FrameText(thread.stackHits[hit])));
        }
    }
}


std::string Libraries(bool& truncated, std::size_t budget) {
    std::string list = "[";
    const std::size_t count = (std::min)(g_libraryCount.load(), MaxLibraries);
    bool first = true;
    for (std::size_t i = 0; i < count; ++i) {
        const char* name = g_libraries[i].load();
        if (name == nullptr) {
            continue;
        }
        const std::string entry = Json(name);
        if (list.size() + entry.size() + 2 > budget) {
            truncated = true;
            break;
        }
        list += (first ? "" : ", ") + entry;
        first = false;
    }
    return list + "]";
}

std::string Truncate(const char* text, std::size_t limit) {
    if (text == nullptr) {
        return {};
    }
    std::string value(text);
    if (value.size() > limit) {
        value.resize(limit);
    }
    return value;
}

std::string BuildReport(const AbxStop& stop) {
    const abx::TitleConfig& title = Title();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    bool truncated = false;
    std::string exception = "null";
    if (stop.hasException) {
        std::uint64_t offset = 0;
        const std::string module = ModuleOf(stop.exceptionAddress, offset);
        exception = std::format("{{\"code\": \"0x{:08x}\", \"address\": \"0x{:x}\", \"module\": {}, \"offset\": {}}}",
            stop.exceptionCode, stop.exceptionAddress, JsonOrNull(module), module.empty() ? "null" : std::format("\"0x{:x}\"", offset));
    }
    const std::string location = stop.file != nullptr ? std::format("{}:{}", stop.file, stop.line) : std::string{};
    const char* modified = ANYBOX_MODIFIED < 0 ? "null" : (ANYBOX_MODIFIED ? "true" : "false");
    std::string head = std::format(
        "{{\n  \"format\": \"anybox-report\",\n  \"version\": 1,\n  \"generated\": \"{}\",\n"
        "  \"anybox\": {{\"commit\": {}, \"release\": {}, \"modified\": {}}},\n"
        "  \"game\": {{\"titleId\": {}, \"identityName\": {}, \"version\": {}, \"name\": {}}},\n"
        "  \"system\": {{\"os\": \"windows\", \"osVersion\": {}, \"cpu\": {}, \"ramGiB\": {}, \"gpu\": null}},\n"
        "  \"run\": {{\"seconds\": {:.3f}, \"exitCode\": null}},\n",
        UtcNow(), JsonOrNull(ANYBOX_COMMIT), JsonOrNull(ANYBOX_RELEASE), modified,
        JsonOrNull(title.titleId), JsonOrNull(Truncate(title.identityName.c_str(), 256)), JsonOrNull(title.version),
        JsonOrNull(Truncate(title.name.c_str(), 256)), JsonOrNull(OsVersion()), JsonOrNull(Truncate(CpuName().c_str(), 256)), RamGiB(), seconds);
    std::string frames = "[";
    const std::size_t frameCount = stop.frames != nullptr ? (std::min)(stop.frameCount, MaxFrames) : 0;
    for (std::size_t index = 0; index < frameCount; ++index) frames += (index == 0 ? "" : ", ") + FrameJson(stop.frames[index]);
    frames += "]";
    const std::size_t caller = CallerFrame(stop.frames, frameCount);
    const std::string callerJson = caller < frameCount ? FrameJson(stop.frames[caller]) : "null";
    const std::string tail = std::format(
        "  \"stop\": {{\"kind\": \"{}\", \"library\": {}, \"function\": {}, \"location\": {}, \"message\": {}, \"exception\": {}, \"caller\": {}, \"frames\": {}}}\n}}\n",
        stop.kind, JsonOrNull(Truncate(stop.library, 256)), JsonOrNull(Truncate(stop.function, 256)), JsonOrNull(Truncate(location.c_str(), 512)),
        JsonOrNull(Truncate(stop.message, 4096)), exception, callerJson, frames);
    const std::size_t reserved = head.size() + tail.size() + 64;
    const std::string libraries = Libraries(truncated, reserved < MaxReportBytes ? MaxReportBytes - reserved : 0);
    return head + "  \"libraries\": " + libraries + ",\n  \"truncated\": " + (truncated ? "true" : "false") + ",\n" + tail;
}

std::wstring ReportPath() {
    const std::string configured = Environment("ANYBOX_REPORT");
    if (!configured.empty()) {
        const int length = MultiByteToWideChar(CP_UTF8, 0, configured.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, configured.c_str(), -1, wide.data(), length);
        wide.resize(static_cast<std::size_t>(length - 1));
        return wide;
    }
    return ExecutableDirectory() + L"\\anybox-report.json";
}

void Emit(const std::string& line) {
    std::lock_guard lock(g_logMutex);
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
    if (g_logFile != nullptr) {
        std::fputs(line.c_str(), g_logFile);
        std::fflush(g_logFile);
    }
}

void EmitFrames(const char* what, const std::uint64_t* frames, std::size_t count) {
    const std::size_t caller = CallerFrame(frames, count);
    if (caller < count) Emit(std::format("[anybox] {} {}\n", what, FrameText(frames[caller])));
    for (std::size_t index = 0; index < count; ++index) Emit(std::format("[anybox]   #{:02} {}\n", index, FrameText(frames[index])));
}

bool BreakEnabled() {
    return IsDebuggerPresent() && Environment("ANYBOX_BREAK") != "0";
}

void WaitForDebugger() {
    const std::string configured = Environment("ANYBOX_WAIT_DEBUGGER");
    if (configured.empty()) {
        return;
    }
    const unsigned long seconds = std::strtoul(configured.c_str(), nullptr, 10);
    Emit(std::format("[anybox] waiting up to {} s for a debugger to attach to process {}\n", seconds, GetCurrentProcessId()));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!IsDebuggerPresent() && std::chrono::steady_clock::now() < deadline) {
        Sleep(100);
    }
    if (IsDebuggerPresent()) {
        Emit("[anybox] debugger attached; breaking before the title starts\n");
        DebugBreak();
    } else {
        Emit("[anybox] no debugger attached; continuing\n");
    }
}

std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_previousFilter{};
std::atomic<bool> g_inFilter{};
std::atomic<DWORD> g_handledThread{};
PVOID g_keepFilter = nullptr;
HANDLE g_rawCrash = INVALID_HANDLE_VALUE;

void RawHex(char*& out, const char* label, std::uint64_t value) {
    while (*label != '\0') *out++ = *label++;
    for (int shift = 60; shift >= 0; shift -= 4) *out++ = "0123456789abcdef"[(value >> shift) & 0xF];
}

void RawModuleOffset(char*& out, const char* end, std::uint64_t address) {
    HMODULE module = nullptr;
    if (address == 0 || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &module) ||
        module == nullptr) {
        if (out + 20 < end) RawHex(out, " ", address);
        return;
    }
    char name[MAX_PATH];
    const DWORD length = GetModuleFileNameA(module, name, MAX_PATH);
    const char* base = name;
    for (DWORD i = 0; i < length; ++i) {
        if (name[i] == '\\' || name[i] == '/') base = name + i + 1;
    }
    if (out < end) *out++ = ' ';
    for (const char* c = base; c < name + length && out < end; ++c) *out++ = *c;
    if (out + 20 < end) RawHex(out, "+", address - reinterpret_cast<std::uint64_t>(module));
}

using ThreadQuery = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

void RawCrashLine(const EXCEPTION_POINTERS* pointers, const char* kind) {
    if (g_rawCrash == INVALID_HANDLE_VALUE) return;
    char line[2048];
    char* out = line;
    const EXCEPTION_RECORD* record = pointers != nullptr ? pointers->ExceptionRecord : nullptr;
    RawHex(out, kind, record != nullptr ? record->ExceptionCode : 0);
    RawHex(out, " at ", record != nullptr ? reinterpret_cast<std::uintptr_t>(record->ExceptionAddress) : 0);
    RawHex(out, " info0 ", record != nullptr && record->NumberParameters >= 1 ? record->ExceptionInformation[0] : 0);
    RawHex(out, " info1 ", record != nullptr && record->NumberParameters >= 2 ? record->ExceptionInformation[1] : 0);
    RawHex(out, " rsp ", pointers != nullptr && pointers->ContextRecord != nullptr ? pointers->ContextRecord->Rsp : 0);
    RawHex(out, " thread ", GetCurrentThreadId());
    RawHex(out, " title ", reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
    const char* const end = line + sizeof(line) - 2;
    static const ThreadQuery query = reinterpret_cast<ThreadQuery>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    void* start = nullptr;
    if (query != nullptr && query(GetCurrentThread(), 9, &start, sizeof(start), nullptr) == 0) {
        const char* label = " thread-start";
        while (*label != '\0' && out < end) *out++ = *label++;
        RawModuleOffset(out, end, reinterpret_cast<std::uint64_t>(start));
    }
    if (pointers != nullptr && pointers->ContextRecord != nullptr) {
        std::uint64_t frames[MaxFrames]{};
        const std::size_t count = UnwindFrames(*pointers->ContextRecord, frames);
        const char* label = " frames";
        while (*label != '\0' && out < end) *out++ = *label++;
        for (std::size_t frame = 0; frame < count && frame < 16; ++frame) RawModuleOffset(out, end, frames[frame]);
    }
    *out++ = '\n';
    DWORD written = 0;
    WriteFile(g_rawCrash, line, static_cast<DWORD>(out - line), &written, nullptr);
    FlushFileBuffers(g_rawCrash);
}

void EmitPointedMemory(const CONTEXT& c) {
    struct Named {
        const char* name;
        std::uint64_t value;
    };
    const Named registers[] = {{"rax", c.Rax}, {"rbx", c.Rbx}, {"rcx", c.Rcx}, {"rdx", c.Rdx}, {"rsi", c.Rsi}, {"rdi", c.Rdi}, {"rbp", c.Rbp}, {"r8", c.R8},
                               {"r9", c.R9},   {"r10", c.R10}, {"r11", c.R11}, {"r12", c.R12}, {"r13", c.R13}, {"r14", c.R14}, {"r15", c.R15}};
    constexpr std::size_t kWordsBefore = 2;
    constexpr std::size_t kWords = 8;
    std::size_t index = 0;
    for (const Named& one : registers) {
        bool repeated = false;
        for (std::size_t earlier = 0; earlier < index; ++earlier) repeated = repeated || registers[earlier].value == one.value;
        ++index;
        if (repeated || one.value < 0x10000) continue;
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(one.value), &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 || region.Type == MEM_IMAGE) {
            continue;
        }
        const std::uint64_t regionStart = reinterpret_cast<std::uint64_t>(region.BaseAddress);
        const std::uint64_t regionEnd = regionStart + region.RegionSize;
        std::uint64_t start = one.value & ~std::uint64_t{7};
        for (std::size_t back = 0; back < kWordsBefore && start >= regionStart + 8; ++back) start -= 8;
        std::uint64_t words[kWords]{};
        const std::size_t count = static_cast<std::size_t>((std::min)(std::uint64_t{kWords}, (regionEnd - start) / 8));
        SIZE_T read = 0;
        if (count == 0 || !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(start), words, count * 8, &read) || read != count * 8) continue;
        std::string line = std::format("[anybox]   {} points into {} memory (allocation {:x}, region {:x}..{:x}, protection 0x{:x}); from {:x}:", one.name,
                                       region.Type == MEM_MAPPED ? "mapped" : "private", reinterpret_cast<std::uint64_t>(region.AllocationBase), regionStart, regionEnd,
                                       static_cast<unsigned>(region.Protect), start);
        for (std::size_t word = 0; word < count; ++word) line += std::format(" {:016x}", words[word]);
        Emit(line + "\n");
    }
}

LONG WINAPI UnhandledException(EXCEPTION_POINTERS* pointers) {
    if (g_inFilter.exchange(true)) {
        RawCrashLine(pointers, "nested crash code ");
        return EXCEPTION_CONTINUE_SEARCH;
    }
    RawCrashLine(pointers, "crash code ");
    if (g_handledThread.load() == GetCurrentThreadId()) {
        g_inFilter.store(false);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const EXCEPTION_RECORD* record = pointers != nullptr ? pointers->ExceptionRecord : nullptr;
    AbxStop stop{};
    stop.kind = "crash";
    std::uint64_t frames[MaxFrames]{};
    if (record != nullptr) {
        stop.hasException = true;
        stop.exceptionCode = record->ExceptionCode;
        stop.exceptionAddress = reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);
        if (record->ExceptionCode == 0x20474343u) {
            stop.message = "uncaught C++ exception thrown by an AnyBox library";
        }
        Emit(std::format("[anybox] crash 0x{:08x} at {}\n", stop.exceptionCode, FrameText(stop.exceptionAddress)));
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
            Emit(std::format("[anybox]   {} of 0x{:x}\n", record->ExceptionInformation[0] == 0 ? "read" : record->ExceptionInformation[0] == 1 ? "write" : "execute",
                             record->ExceptionInformation[1]));
        }
    }
    if (pointers != nullptr && pointers->ContextRecord != nullptr) {
        const CONTEXT& c = *pointers->ContextRecord;
        Emit(std::format("[anybox]   rax {:016x} rbx {:016x} rcx {:016x} rdx {:016x} rsi {:016x} rdi {:016x} rbp {:016x} rsp {:016x}\n", c.Rax, c.Rbx, c.Rcx, c.Rdx,
                         c.Rsi, c.Rdi, c.Rbp, c.Rsp));
        Emit(std::format("[anybox]   r8 {:016x} r9 {:016x} r10 {:016x} r11 {:016x} r12 {:016x} r13 {:016x} r14 {:016x} r15 {:016x}\n", c.R8, c.R9, c.R10, c.R11, c.R12,
                         c.R13, c.R14, c.R15));
        EmitPointedMemory(c);
    }
    if (pointers != nullptr && pointers->ContextRecord != nullptr) {
        stop.frames = frames;
        stop.frameCount = UnwindFrames(*pointers->ContextRecord, frames);
        EmitFrames("crashed in", frames, stop.frameCount);
        if (Environment("ANYBOX_CRASH_THREADS") == "1") EmitOtherThreads();
    }
    AbxWriteReport(&stop);
    const LPTOP_LEVEL_EXCEPTION_FILTER previous = g_previousFilter.load();
    const LONG result = previous != nullptr ? previous(pointers) : EXCEPTION_CONTINUE_SEARCH;
    if (result == EXCEPTION_CONTINUE_EXECUTION) {
        g_reportWritten.store(false);
    }
    g_handledThread.store(GetCurrentThreadId());
    g_inFilter.store(false);
    return result;
}

std::atomic<int> g_firstChanceLogged{0};

LONG WINAPI KeepFilter(EXCEPTION_POINTERS* pointers) {
    const DWORD code = pointers != nullptr && pointers->ExceptionRecord != nullptr ? pointers->ExceptionRecord->ExceptionCode : 0;
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW || code == 0xC0000374u) && g_firstChanceLogged.fetch_add(1) < 64) {
        RawCrashLine(pointers, "first-chance code ");
    }
    if (!g_inFilter.load() && g_handledThread.load() == GetCurrentThreadId()) {
        g_handledThread.store(0);
    }
    const LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(UnhandledException);
    if (previous != UnhandledException) {
        g_previousFilter.store(previous);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

std::atomic<std::uint64_t> g_watchAddress{0};
std::atomic<int> g_watchHits{0};

LONG CALLBACK WatchWriteHandler(EXCEPTION_POINTERS* info) {
    if (info == nullptr || info->ExceptionRecord == nullptr || info->ContextRecord == nullptr) return EXCEPTION_CONTINUE_SEARCH;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || (info->ContextRecord->Dr6 & 1u) == 0) return EXCEPTION_CONTINUE_SEARCH;
    info->ContextRecord->Dr6 = 0;
    const std::uint64_t address = g_watchAddress.load();
    if (g_watchHits.fetch_add(1) < 16) {
        std::uint64_t frames[MaxFrames]{};
        const std::size_t count = UnwindFrames(*info->ContextRecord, frames);
        const std::uint64_t value = address != 0 ? *reinterpret_cast<const volatile std::uint64_t*>(address) : 0;
        Emit(std::format("[anybox] watched address 0x{:x} written (now {}) by thread {}:\n", address, value, GetCurrentThreadId()));
        for (std::size_t frame = 0; frame < (std::min)(count, std::size_t{8}); ++frame) Emit(std::format("[anybox]     #{:02} {}\n", frame, FrameText(frames[frame])));
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

void ArmWatch() {
    const std::uint64_t address = g_watchAddress.load();
    if (address == 0) return;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD process = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
        const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, entry.th32ThreadID);
        if (thread == nullptr) continue;
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &context) && context.Dr0 != address) {
                context.Dr0 = address;
                context.Dr7 = (context.Dr7 & ~static_cast<DWORD64>(0xF0003)) | 1u | (1u << 16) | (3u << 18);
                SetThreadContext(thread, &context);
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
}

DWORD WINAPI HoldTitleWatchdogPaused(LPVOID parameter) {
    auto* const flag = static_cast<volatile std::uint8_t*>(parameter);
    for (;;) {
        *flag = 1;
        Sleep(250);
    }
}

void StartTitleWatchdogPause() {
    const char* value = AbxSetting("runtime.title_watchdog_pause_rva");
    if (value == nullptr || value[0] == 0) return;
    const std::uint64_t rva = std::strtoull(value, nullptr, 16);
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    MEMORY_BASIC_INFORMATION region{};
    const bool queried = rva != 0 && VirtualQuery(reinterpret_cast<const void*>(base + rva), &region, sizeof(region)) == sizeof(region);
    if (!queried || region.AllocationBase != reinterpret_cast<void*>(base) || region.State != MEM_COMMIT || (region.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) == 0) {
        Emit(std::format("[anybox] runtime.title_watchdog_pause_rva = 0x{:x} is not writable data of the title's image; the title's watchdog is left running\n", rva));
        return;
    }
    Emit(std::format("[anybox] the title's hang watchdog is held paused through its own pause flag at title+0x{:x} (runtime.title_watchdog_pause_rva): frames that take longer than its "
                     "limit no longer end the process\n", rva));
    if (const HANDLE holder = CreateThread(nullptr, 0, HoldTitleWatchdogPaused, reinterpret_cast<void*>(base + rva), 0, nullptr)) CloseHandle(holder);
}

DWORD WINAPI WatchDumpRequests(LPVOID) {
    wchar_t module[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    std::wstring path(module, length);
    const std::size_t slash = path.find_last_of(L"\\/");
    path = (slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash)) + L"\\anybox_dump_threads";
    for (;;) {
        Sleep(2000);
        ArmWatch();
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        DeleteFileW(path.c_str());
        EmitOtherThreads("on request (anybox_dump_threads)");
    }
}

void Start() {
    g_start = std::chrono::steady_clock::now();
    const std::string logPath = Environment("ANYBOX_LOG_FILE");
    if (!logPath.empty()) {
        g_logFile = std::fopen(logPath.c_str(), "a");
    }
    g_rawCrash = CreateFileW((ExecutableDirectory() + L"\\anybox-crash-raw.txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    SetUnhandledExceptionFilter(UnhandledException);
    g_keepFilter = AddVectoredExceptionHandler(0, KeepFilter);
    abx::InstallCpuFixups();
    abx::InstallProbes();
    abx::InstallTitleAssertPolicy();
    abx::InstallLoaderRedirect();
    if (const HANDLE watcher = CreateThread(nullptr, 0, WatchDumpRequests, nullptr, 0, nullptr)) CloseHandle(watcher);
    StartTitleWatchdogPause();
    WaitForDebugger();
}

}

ABX_RUNTIME_API void AbxRegisterLibrary(const char* library) {
    const std::size_t index = g_libraryCount.fetch_add(1);
    if (index < MaxLibraries) {
        g_libraries[index].store(library);
    }
}

ABX_RUNTIME_API bool AbxWriteReport(const AbxStop* stop) {
    if (g_reportWritten.exchange(true)) {
        return false;
    }
    if (!Environment("ANYBOX_NO_REPORT").empty()) {
        return false;
    }
    const std::string report = BuildReport(*stop);
    if (report.size() > MaxReportBytes) {
        return false;
    }
    const HANDLE file = CreateFileW(ReportPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, report.data(), static_cast<DWORD>(report.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == report.size();
}

ABX_RUNTIME_API bool AbxTraceEnabled(const char* library) {
    static const std::vector<std::string> names = TraceNames();
    if (library == nullptr) {
        return false;
    }
    std::string wanted(library);
    for (char& c : wanted) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (const std::string& name : names) {
        if (name == "all" || name == wanted) {
            return true;
        }
    }
    return false;
}

ABX_RUNTIME_API void AbxLog(const char* channel, const char* format, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Emit(std::format("[{}] {}\n", channel, buffer));
}

ABX_RUNTIME_API void AbxNotImplementedAt [[noreturn]] (const char* library, const char* function, const char* file, int line) {
    Emit(std::format("[anybox] not implemented: {}!{} ({}:{})\n", library, function, file, line));
    std::uint64_t frames[MaxFrames]{};
    AbxStop stop{};
    stop.kind = "not-implemented";
    stop.library = library;
    stop.function = function;
    stop.file = file;
    stop.line = line;
    stop.frames = frames;
    stop.frameCount = CaptureFrames(frames);
    EmitFrames("called from", frames, stop.frameCount);
    AbxWriteReport(&stop);
    if (BreakEnabled()) DebugBreak();
    TerminateProcess(GetCurrentProcess(), AbxExitNotImplemented);
    std::abort();
}

ABX_RUNTIME_API void AbxUnsupportedAt [[noreturn]] (const char* library, const char* function, const char* message, const char* file, int line) {
    Emit(std::format("[anybox] unsupported: {}!{}: {} ({}:{})\n", library, function, message, file, line));
    std::uint64_t frames[MaxFrames]{};
    AbxStop stop{};
    stop.kind = "unsupported";
    stop.library = library;
    stop.function = function;
    stop.file = file;
    stop.line = line;
    stop.message = message;
    stop.frames = frames;
    stop.frameCount = CaptureFrames(frames);
    EmitFrames("called from", frames, stop.frameCount);
    AbxWriteReport(&stop);
    if (BreakEnabled()) DebugBreak();
    TerminateProcess(GetCurrentProcess(), AbxExitUnsupported);
    std::abort();
}

namespace {

std::wstring Widen(const std::string& text) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), length);
    wide.resize(static_cast<std::size_t>(length - 1));
    return wide;
}

void LoadSettings() {
    const std::string configured = Environment("ANYBOX_CONFIG");
    const std::filesystem::path path = configured.empty() ? std::filesystem::path(ExecutableDirectory() + L"\\anybox.cfg") : std::filesystem::path(Widen(configured));
    std::vector<std::string> problems;
    std::ifstream file(path, std::ios::binary);
    if (file) {
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        abx::ParseSettings(text, path.filename().string(), g_settings, problems);
        Emit(std::format("[anybox] read {} setting(s) from {}\n", g_settings.size(), path.string()));
    } else if (!configured.empty()) {
        problems.push_back("ANYBOX_CONFIG names a file that cannot be read: " + configured);
    }
    abx::ParseSettingOverrides(Environment("ANYBOX_SET"), g_settings, problems);
    for (const std::string& problem : problems) Emit(std::format("[anybox] settings: {}\n", problem));
}

}

ABX_RUNTIME_API const char* AbxSetting(const char* key) {
    std::call_once(g_settingsOnce, LoadSettings);
    if (key == nullptr) {
        return nullptr;
    }
    const auto found = g_settings.find(key);
    if (found == g_settings.end()) {
        return nullptr;
    }
    {
        std::lock_guard lock(g_settingsMutex);
        if (g_settingsLogged.insert(found->first).second) {
            Emit(std::format("[anybox] setting {} = {} (from {})\n", found->first, found->second.value, found->second.source));
        }
    }
    return found->second.value.c_str();
}

ABX_RUNTIME_API const char* AbxTitleId() {
    return Title().titleId.empty() ? nullptr : Title().titleId.c_str();
}

ABX_RUNTIME_API const char* AbxTitleName() {
    return Title().name.empty() ? nullptr : Title().name.c_str();
}

ABX_RUNTIME_API const char* AbxTitleDeviceFamily() {
    return Title().targetDeviceFamily.empty() ? nullptr : Title().targetDeviceFamily.c_str();
}

ABX_API void AbxWatchWrites(std::uint64_t address) {
    if (address == 0 || address % 8 != 0 || g_watchAddress.exchange(address) != 0) return;
    AddVectoredExceptionHandler(1, WatchWriteHandler);
    Emit(std::format("[anybox] hardware write watch armed on 0x{:x} in every thread (re-armed every 2 s for new threads)\n", address));
    ArmWatch();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        Start();
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr && g_keepFilter != nullptr) {
        RemoveVectoredExceptionHandler(g_keepFilter);
    } else if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
        AbxStop stop{};
        stop.kind = "none";
        AbxWriteReport(&stop);
    }
    return TRUE;
}
