#include "ExitWatch.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>

#if defined(_MSC_VER)
#include <intrin.h>
#define ABX_CALLER _ReturnAddress()
#else
#define ABX_CALLER __builtin_return_address(0)
#endif

namespace abx {

namespace {

using ExitProcessFunction = VOID(WINAPI*)(UINT);
using TerminateProcessFunction = BOOL(WINAPI*)(HANDLE, UINT);
using ExitThreadFunction = VOID(WINAPI*)(DWORD);
using RaiseFailFastExceptionFunction = VOID(WINAPI*)(PEXCEPTION_RECORD, PCONTEXT, DWORD);
using StatusExitFunction = VOID(NTAPI*)(LONG);
using NtTerminateProcessFunction = LONG(NTAPI*)(HANDLE, LONG);
using CrtExitFunction = void(__cdecl*)(int);
using CrtAbortFunction = void(__cdecl*)();

constexpr int kNotReported = 0;
constexpr int kReporting = 1;
constexpr int kReported = 2;
constexpr DWORD kReportWaitMs = 2000;

constexpr std::uint8_t kStubHead[] = {0x4C, 0x8B, 0xD1, 0xB8};
constexpr std::uint8_t kStubTail[] = {0xF6, 0x04, 0x25, 0x08, 0x03, 0xFE, 0x7F, 0x01, 0x75, 0x03, 0x0F, 0x05, 0xC3, 0xCD, 0x2E, 0xC3};
constexpr std::size_t kStubTailAt = 8;
constexpr std::size_t kStubSize = kStubTailAt + sizeof(kStubTail);
constexpr std::size_t kJumpSize = 14;

std::atomic<ExitReporter> g_reporter{nullptr};
std::atomic<int> g_processState{kNotReported};
std::atomic<DWORD> g_reportingThread{0};
std::atomic<bool> g_mainThreadReported{false};
std::atomic<DWORD> g_mainThread{0};
HANDLE g_mainThreadHandle = nullptr;

ExitProcessFunction g_exitProcess = nullptr;
TerminateProcessFunction g_terminateProcess = nullptr;
ExitThreadFunction g_exitThread = nullptr;
RaiseFailFastExceptionFunction g_raiseFailFast = nullptr;
StatusExitFunction g_rtlExitUserProcess = nullptr;
StatusExitFunction g_rtlExitUserThread = nullptr;
NtTerminateProcessFunction g_ntTerminateProcess = nullptr;
CrtExitFunction g_crtExit = nullptr;
CrtExitFunction g_crtUnderscoreExit = nullptr;
CrtExitFunction g_crtCapitalExit = nullptr;
CrtExitFunction g_crtQuickExit = nullptr;
CrtAbortFunction g_crtAbort = nullptr;
CrtAbortFunction g_crtTerminate = nullptr;

bool ClaimProcessReport() {
    int expected = kNotReported;
    if (g_processState.compare_exchange_strong(expected, kReporting)) {
        g_reportingThread.store(GetCurrentThreadId());
        return true;
    }
    if (expected == kReporting && g_reportingThread.load() != GetCurrentThreadId()) {
        for (DWORD waited = 0; waited < kReportWaitMs && g_processState.load() == kReporting; waited += 10) Sleep(10);
    }
    return false;
}

void ReportProcessEnd(const char* how, const char* target, std::uint32_t code, bool hasCode, void* caller, bool late = false) {
    if (!ClaimProcessReport()) return;
    if (const ExitReporter reporter = g_reporter.load()) reporter(ExitEvent{how, target, code, hasCode, true, true, late, GetCurrentThreadId(), caller});
    g_processState.store(kReported);
}

void ReportMainThreadEnd(const char* how, std::uint32_t code, bool stack, void* caller) {
    if (g_processState.load() != kNotReported || g_mainThreadReported.exchange(true)) return;
    if (const ExitReporter reporter = g_reporter.load()) reporter(ExitEvent{how, "the title's main thread", code, true, false, stack, false, g_mainThread.load(), caller});
}

bool IsThisProcess(HANDLE process) {
    if (process == GetCurrentProcess()) return true;
    if (process == nullptr) return false;
    const DWORD id = GetProcessId(process);
    return id != 0 && id == GetCurrentProcessId();
}

bool OnMainThread() {
    return GetCurrentThreadId() == g_mainThread.load();
}

VOID WINAPI RedirectExitProcess(UINT code) {
    ReportProcessEnd("ExitProcess", "this process", code, true, ABX_CALLER);
    g_exitProcess(code);
}

BOOL WINAPI RedirectTerminateProcess(HANDLE process, UINT code) {
    if (IsThisProcess(process)) ReportProcessEnd("TerminateProcess", "this process", code, true, ABX_CALLER);
    return g_terminateProcess(process, code);
}

VOID WINAPI RedirectExitThread(DWORD code) {
    if (OnMainThread()) ReportMainThreadEnd("ExitThread", code, true, ABX_CALLER);
    g_exitThread(code);
}

VOID WINAPI RedirectRaiseFailFastException(PEXCEPTION_RECORD record, PCONTEXT context, DWORD flags) {
    ReportProcessEnd("RaiseFailFastException", "this process", record != nullptr ? static_cast<std::uint32_t>(record->ExceptionCode) : 0, record != nullptr, ABX_CALLER);
    g_raiseFailFast(record, context, flags);
}

VOID NTAPI RedirectRtlExitUserProcess(LONG status) {
    ReportProcessEnd("RtlExitUserProcess", "this process", static_cast<std::uint32_t>(status), true, ABX_CALLER);
    g_rtlExitUserProcess(status);
}

VOID NTAPI RedirectRtlExitUserThread(LONG status) {
    if (OnMainThread()) ReportMainThreadEnd("RtlExitUserThread", static_cast<std::uint32_t>(status), true, ABX_CALLER);
    g_rtlExitUserThread(status);
}

void __cdecl RedirectCrtExit(int code) {
    ReportProcessEnd("exit", "this process", static_cast<std::uint32_t>(code), true, ABX_CALLER);
    g_crtExit(code);
}

void __cdecl RedirectCrtUnderscoreExit(int code) {
    ReportProcessEnd("_exit", "this process", static_cast<std::uint32_t>(code), true, ABX_CALLER);
    g_crtUnderscoreExit(code);
}

void __cdecl RedirectCrtCapitalExit(int code) {
    ReportProcessEnd("_Exit", "this process", static_cast<std::uint32_t>(code), true, ABX_CALLER);
    g_crtCapitalExit(code);
}

void __cdecl RedirectCrtQuickExit(int code) {
    ReportProcessEnd("quick_exit", "this process", static_cast<std::uint32_t>(code), true, ABX_CALLER);
    g_crtQuickExit(code);
}

void __cdecl RedirectCrtAbort() {
    ReportProcessEnd("abort", "this process", 0, false, ABX_CALLER);
    g_crtAbort();
}

void __cdecl RedirectCrtTerminate() {
    ReportProcessEnd("terminate", "this process", 0, false, ABX_CALLER);
    g_crtTerminate();
}

LONG NTAPI HookedNtTerminateProcess(HANDLE process, LONG status) {
    if (process == nullptr) {
        ReportProcessEnd("NtTerminateProcess", "every other thread of this process (the first step of ending it)", static_cast<std::uint32_t>(status), true, ABX_CALLER);
    } else if (IsThisProcess(process)) {
        ReportProcessEnd("NtTerminateProcess", "this process", static_cast<std::uint32_t>(status), true, ABX_CALLER);
    }
    return g_ntTerminateProcess(process, status);
}

DWORD WINAPI WaitForMainThread(LPVOID) {
    if (WaitForSingleObject(g_mainThreadHandle, INFINITE) != WAIT_OBJECT_0) return 0;
    DWORD code = 0;
    if (GetExitCodeThread(g_mainThreadHandle, &code)) ReportMainThreadEnd("its end, seen by a thread that waits on it", code, false, nullptr);
    return 0;
}

DWORD EarliestThread() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD process = GetCurrentProcessId();
    DWORD earliest = 0;
    ULONGLONG earliestTime = ~0ull;
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process) continue;
        const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (thread == nullptr) continue;
        FILETIME created{};
        FILETIME exited{};
        FILETIME kernel{};
        FILETIME user{};
        if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
            const ULONGLONG time = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            if (time < earliestTime) {
                earliestTime = time;
                earliest = entry.th32ThreadID;
            }
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return earliest;
}

template <typename Function>
void AddSystem(const std::function<void(void*, void*)>& add, std::initializer_list<HMODULE> modules, const char* name, Function& call, void* replacement) {
    void* found[2] = {};
    std::size_t count = 0;
    for (const HMODULE module : modules) {
        if (module == nullptr || count == 2) continue;
        void* address = reinterpret_cast<void*>(GetProcAddress(module, name));
        if (address != nullptr && (count == 0 || found[0] != address)) found[count++] = address;
    }
    if (count == 0) return;
    call = reinterpret_cast<Function>(found[count - 1]);
    for (std::size_t index = 0; index < count; ++index) add(found[index], replacement);
}

}

void SetExitReporter(ExitReporter reporter) {
    g_reporter.store(reporter);
}

void AddExitRedirects(const std::function<void(void* original, void* replacement)>& add) {
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
    AddSystem(add, {kernel32, kernelbase}, "ExitProcess", g_exitProcess, reinterpret_cast<void*>(&RedirectExitProcess));
    AddSystem(add, {kernel32, kernelbase}, "TerminateProcess", g_terminateProcess, reinterpret_cast<void*>(&RedirectTerminateProcess));
    AddSystem(add, {kernel32, kernelbase}, "ExitThread", g_exitThread, reinterpret_cast<void*>(&RedirectExitThread));
    AddSystem(add, {kernel32, kernelbase}, "RaiseFailFastException", g_raiseFailFast, reinterpret_cast<void*>(&RedirectRaiseFailFastException));
    AddSystem(add, {ntdll}, "RtlExitUserProcess", g_rtlExitUserProcess, reinterpret_cast<void*>(&RedirectRtlExitUserProcess));
    AddSystem(add, {ntdll}, "RtlExitUserThread", g_rtlExitUserThread, reinterpret_cast<void*>(&RedirectRtlExitUserThread));
    AddSystem(add, {ucrt}, "exit", g_crtExit, reinterpret_cast<void*>(&RedirectCrtExit));
    AddSystem(add, {ucrt}, "_exit", g_crtUnderscoreExit, reinterpret_cast<void*>(&RedirectCrtUnderscoreExit));
    AddSystem(add, {ucrt}, "_Exit", g_crtCapitalExit, reinterpret_cast<void*>(&RedirectCrtCapitalExit));
    AddSystem(add, {ucrt}, "quick_exit", g_crtQuickExit, reinterpret_cast<void*>(&RedirectCrtQuickExit));
    AddSystem(add, {ucrt}, "abort", g_crtAbort, reinterpret_cast<void*>(&RedirectCrtAbort));
    AddSystem(add, {ucrt}, "terminate", g_crtTerminate, reinterpret_cast<void*>(&RedirectCrtTerminate));
}

const char* InstallTerminateHook() {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) return "ntdll.dll is not loaded";
    auto* const stub = reinterpret_cast<std::uint8_t*>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtTerminateProcess")));
    if (stub == nullptr) return "ntdll.dll does not export it";
    if (std::memcmp(stub, kStubHead, sizeof(kStubHead)) != 0 || std::memcmp(stub + kStubTailAt, kStubTail, sizeof(kStubTail)) != 0) {
        return "its code is not the system call stub this hook was written for (another hook or a debugger breakpoint may be on it)";
    }
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&HookedNtTerminateProcess), &self)) {
        return "AnyBox's runtime library could not be pinned in memory";
    }
    auto* const copy = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kStubSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (copy == nullptr) return "no memory for a copy of the call";
    std::memcpy(copy, stub, kStubSize);
    DWORD old = 0;
    if (!VirtualProtect(copy, kStubSize, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(copy, 0, MEM_RELEASE);
        return "the copy of the call could not be made executable";
    }
    FlushInstructionCache(GetCurrentProcess(), copy, kStubSize);
    g_ntTerminateProcess = reinterpret_cast<NtTerminateProcessFunction>(reinterpret_cast<void*>(copy));
    std::uint8_t jump[kJumpSize] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    const auto target = reinterpret_cast<std::uint64_t>(&HookedNtTerminateProcess);
    std::memcpy(jump + 6, &target, sizeof(target));
    if (!VirtualProtect(stub, kJumpSize, PAGE_EXECUTE_READWRITE, &old)) return "its code could not be made writable";
    std::memcpy(stub, jump, kJumpSize);
    VirtualProtect(stub, kJumpSize, old, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, kJumpSize);
    return nullptr;
}

void WatchMainThread(bool loadedAtStart) {
    const DWORD id = loadedAtStart ? GetCurrentThreadId() : EarliestThread();
    if (id == 0) return;
    g_mainThread.store(id);
    g_mainThreadHandle = OpenThread(SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
    if (g_mainThreadHandle == nullptr) return;
    if (const HANDLE waiter = CreateThread(nullptr, 0, WaitForMainThread, nullptr, 0, nullptr)) CloseHandle(waiter);
}

DWORD MainThread() {
    return g_mainThread.load();
}

bool ProcessExitReported() {
    return g_processState.load() != kNotReported;
}

void ReportShutdownUnseen() {
    ReportProcessEnd("process shutdown, seen only when AnyBox's runtime library was unloaded (the call that ended the process was not seen)", "this process", 0, false, nullptr, true);
}

}
