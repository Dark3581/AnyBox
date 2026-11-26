#include "BufferRedirect.hpp"

#include "AbxApi.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(_MSC_VER)
#include <intrin.h>
#define ABX_CALLER _ReturnAddress()
#else
#define ABX_CALLER __builtin_return_address(0)
#endif

namespace abx {

namespace {

using ReadFileFunction = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using WriteFileFunction = BOOL(WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);

constexpr int kRepeats = 3;
constexpr std::uint64_t kLoggedAlways = 16;

ReadFileFunction g_readFile = nullptr;
WriteFileFunction g_writeFile = nullptr;
SRWLOCK g_lock = SRWLOCK_INIT;
AbxRefusedBuffer g_handler = nullptr;
void* g_context = nullptr;
std::atomic<bool> g_watched{false};
std::atomic<std::uint64_t> g_repeated{0};
std::atomic<std::uint64_t> g_stillRefused{0};

bool HandBack(const void* buffer, DWORD bytes, bool write, void* caller) {
    if (buffer == nullptr || bytes == 0 || !g_watched.load(std::memory_order_acquire)) return false;
    AcquireSRWLockShared(&g_lock);
    const bool handedBack =
        g_handler != nullptr && g_handler(g_context, reinterpret_cast<std::uint64_t>(buffer), bytes, write, reinterpret_cast<std::uint64_t>(caller));
    ReleaseSRWLockShared(&g_lock);
    return handedBack;
}

void Note(const char* function, const void* buffer, DWORD bytes, bool write, void* caller, int repeats, DWORD first, bool refused) {
    std::atomic<std::uint64_t>& counter = refused ? g_stillRefused : g_repeated;
    const std::uint64_t count = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > kLoggedAlways && (count & (count - 1)) != 0) return;
    char code[MAX_PATH + 32];
    HMODULE module = nullptr;
    char path[MAX_PATH] = {};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(caller), &module) && module != nullptr &&
        GetModuleFileNameA(module, path, sizeof(path)) != 0) {
        const char* name = std::strrchr(path, '\\');
        std::snprintf(code, sizeof(code), "%s+0x%llx", name != nullptr ? name + 1 : path,
                      static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(caller) - reinterpret_cast<std::uintptr_t>(module)));
    } else {
        std::snprintf(code, sizeof(code), "0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(caller)));
    }
    AbxLog("runtime",
           "%s %s the buffer at 0x%llx (%lu bytes), called from %s, failed with error %lu over pages a library keeps guarded; the pages were handed back and the call was "
           "repeated %d time(s) and %s (%llu such call(s) so far; the first %llu and every power of two are logged)",
           function, write ? "into" : "from", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(buffer)), static_cast<unsigned long>(bytes), code,
           static_cast<unsigned long>(first), repeats,
           refused ? "is still refused, so the title gets the failure" : "was not refused again", static_cast<unsigned long long>(count),
           static_cast<unsigned long long>(kLoggedAlways));
}

template <typename Call>
BOOL Repeated(const char* function, const void* buffer, DWORD bytes, bool write, void* caller, const Call& call) {
    DWORD first = ERROR_SUCCESS;
    for (int repeats = 0;; ++repeats) {
        const BOOL result = call();
        if (result != FALSE && repeats == 0) return result;
        const DWORD error = GetLastError();
        const bool refused = result == FALSE && (error == ERROR_NOACCESS || error == ERROR_INVALID_USER_BUFFER);
        if (repeats == 0) first = error;
        if (refused && repeats < kRepeats && HandBack(buffer, bytes, write, caller)) continue;
        if (repeats != 0) Note(function, buffer, bytes, write, caller, repeats, first, refused);
        SetLastError(error);
        return result;
    }
}

BOOL WINAPI RedirectReadFile(HANDLE file, LPVOID buffer, DWORD bytes, LPDWORD read, LPOVERLAPPED overlapped) {
    void* const caller = ABX_CALLER;
    return Repeated("ReadFile", buffer, bytes, true, caller, [&] { return g_readFile(file, buffer, bytes, read, overlapped); });
}

BOOL WINAPI RedirectWriteFile(HANDLE file, LPCVOID buffer, DWORD bytes, LPDWORD written, LPOVERLAPPED overlapped) {
    void* const caller = ABX_CALLER;
    return Repeated("WriteFile", buffer, bytes, false, caller, [&] { return g_writeFile(file, buffer, bytes, written, overlapped); });
}

}

void AddBufferRedirects(const std::function<void(const char*, void*)>& add) {
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32 == nullptr) return;
    const auto readFile = reinterpret_cast<ReadFileFunction>(reinterpret_cast<void*>(GetProcAddress(kernel32, "ReadFile")));
    const auto writeFile = reinterpret_cast<WriteFileFunction>(reinterpret_cast<void*>(GetProcAddress(kernel32, "WriteFile")));
    if (readFile == nullptr || writeFile == nullptr) return;
    g_readFile = readFile;
    g_writeFile = writeFile;
    add("ReadFile", reinterpret_cast<void*>(&RedirectReadFile));
    add("WriteFile", reinterpret_cast<void*>(&RedirectWriteFile));
}

}

ABX_RUNTIME_API bool AbxWatchRefusedBuffers(AbxRefusedBuffer handler, void* context) {
    AcquireSRWLockExclusive(&abx::g_lock);
    abx::g_handler = handler;
    abx::g_context = context;
    abx::g_watched.store(handler != nullptr, std::memory_order_release);
    ReleaseSRWLockExclusive(&abx::g_lock);
    return abx::g_readFile != nullptr;
}
