#include "LoaderRedirect.hpp"

#include "AbxApi.hpp"

#include <windows.h>
#include <winternl.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#define ABX_CALLER _ReturnAddress()
#else
#define ABX_CALLER __builtin_return_address(0)
#endif

namespace abx {

namespace {

struct Route {
    const char* pattern;
    const char* library;
    const char* name;
};

constexpr Route kRoutes[] = {
#include "LibraryMapData.inc"
};

struct DllNotificationData {
    ULONG flags;
    const UNICODE_STRING* fullName;
    const UNICODE_STRING* baseName;
    void* base;
    ULONG size;
};

using DllNotification = VOID(CALLBACK*)(ULONG reason, const DllNotificationData* data, void* context);
using RegisterDllNotification = LONG(NTAPI*)(ULONG flags, DllNotification callback, void* context, void** cookie);

using LoadLibraryExWFunction = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using LoadLibraryExAFunction = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);
using LoadLibraryWFunction = HMODULE(WINAPI*)(LPCWSTR);
using LoadLibraryAFunction = HMODULE(WINAPI*)(LPCSTR);
using GetModuleHandleWFunction = HMODULE(WINAPI*)(LPCWSTR);
using GetModuleHandleAFunction = HMODULE(WINAPI*)(LPCSTR);
using GetModuleHandleExWFunction = BOOL(WINAPI*)(DWORD, LPCWSTR, HMODULE*);
using GetModuleHandleExAFunction = BOOL(WINAPI*)(DWORD, LPCSTR, HMODULE*);

LoadLibraryExWFunction g_loadLibraryExW = nullptr;
LoadLibraryExAFunction g_loadLibraryExA = nullptr;
LoadLibraryWFunction g_loadLibraryW = nullptr;
LoadLibraryAFunction g_loadLibraryA = nullptr;
GetModuleHandleWFunction g_getModuleHandleW = nullptr;
GetModuleHandleAFunction g_getModuleHandleA = nullptr;
GetModuleHandleExWFunction g_getModuleHandleExW = nullptr;
GetModuleHandleExAFunction g_getModuleHandleExA = nullptr;

std::mutex g_logLock;
std::set<std::string> g_logged;

bool Glob(const char* pattern, const std::string& text) {
    const char* p = pattern;
    std::size_t t = 0;
    const char* star = nullptr;
    std::size_t mark = 0;
    while (t < text.size()) {
        if (*p == '*') {
            star = p++;
            mark = t;
        } else if (*p != '\0' && *p == text[t]) {
            ++p;
            ++t;
        } else if (star != nullptr) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (*p == '*') ++p;
    return *p == '\0';
}

std::wstring Widen(const char* text) {
    if (text == nullptr) return {};
    const int length = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, wide.data(), length);
    wide.resize(static_cast<std::size_t>(length - 1));
    return wide;
}

std::wstring RuntimeDirectory() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&RuntimeDirectory), &self);
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring full(path, length);
    const std::size_t slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : full.substr(0, slash + 1);
}

std::wstring Routed(LPCWSTR requested, void* caller, const char* function) {
    if (requested == nullptr) return {};
    const std::string library = RoutedLibrary(requested);
    if (library.empty()) return {};
    std::wstring path = RuntimeDirectory() + Widen(library.c_str());
    std::string from;
    {
        std::lock_guard<std::mutex> guard(g_logLock);
        std::string key(function);
        for (const wchar_t* c = requested; *c != 0; ++c) key.push_back(static_cast<char>(*c < 128 ? *c : '?'));
        if (!g_logged.insert(key).second) return path;
        from = key.substr(std::string(function).size());
    }
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    std::uintptr_t offset = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(caller), &module)) {
        char full[MAX_PATH];
        const DWORD length = GetModuleFileNameA(module, full, MAX_PATH);
        const std::string text(full, length);
        const std::size_t slash = text.find_last_of("\\/");
        lstrcpynA(name, (slash == std::string::npos ? text : text.substr(slash + 1)).c_str(), MAX_PATH);
        offset = reinterpret_cast<std::uintptr_t>(caller) - reinterpret_cast<std::uintptr_t>(module);
    }
    AbxLog("anybox", "loader: %s(%s) from %s+0x%llx loads %s", function, from.c_str(), name, static_cast<unsigned long long>(offset), library.c_str());
    return path;
}

constexpr DWORD kNotACodeLoad = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE;

HMODULE WINAPI RedirectLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    if ((flags & kNotACodeLoad) == 0) {
        const std::wstring path = Routed(name, ABX_CALLER, "LoadLibraryExW");
        if (!path.empty()) return g_loadLibraryExW(path.c_str(), nullptr, 0);
    }
    return g_loadLibraryExW(name, file, flags);
}

HMODULE WINAPI RedirectLoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) {
    if ((flags & kNotACodeLoad) == 0) {
        const std::wstring wide = Widen(name);
        const std::wstring path = Routed(name != nullptr ? wide.c_str() : nullptr, ABX_CALLER, "LoadLibraryExA");
        if (!path.empty()) return g_loadLibraryExW(path.c_str(), nullptr, 0);
    }
    return g_loadLibraryExA(name, file, flags);
}

HMODULE WINAPI RedirectLoadLibraryW(LPCWSTR name) {
    const std::wstring path = Routed(name, ABX_CALLER, "LoadLibraryW");
    return path.empty() ? g_loadLibraryW(name) : g_loadLibraryExW(path.c_str(), nullptr, 0);
}

HMODULE WINAPI RedirectLoadLibraryA(LPCSTR name) {
    const std::wstring wide = Widen(name);
    const std::wstring path = Routed(name != nullptr ? wide.c_str() : nullptr, ABX_CALLER, "LoadLibraryA");
    return path.empty() ? g_loadLibraryA(name) : g_loadLibraryExW(path.c_str(), nullptr, 0);
}

HMODULE WINAPI RedirectGetModuleHandleW(LPCWSTR name) {
    const std::wstring path = Routed(name, ABX_CALLER, "GetModuleHandleW");
    return path.empty() ? g_getModuleHandleW(name) : g_getModuleHandleW(path.c_str());
}

HMODULE WINAPI RedirectGetModuleHandleA(LPCSTR name) {
    const std::wstring wide = Widen(name);
    const std::wstring path = Routed(name != nullptr ? wide.c_str() : nullptr, ABX_CALLER, "GetModuleHandleA");
    return path.empty() ? g_getModuleHandleA(name) : g_getModuleHandleW(path.c_str());
}

BOOL WINAPI RedirectGetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE* module) {
    if ((flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0) {
        const std::wstring path = Routed(name, ABX_CALLER, "GetModuleHandleExW");
        if (!path.empty()) return g_getModuleHandleExW(flags, path.c_str(), module);
    }
    return g_getModuleHandleExW(flags, name, module);
}

BOOL WINAPI RedirectGetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* module) {
    if ((flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0) {
        const std::wstring wide = Widen(name);
        const std::wstring path = Routed(name != nullptr ? wide.c_str() : nullptr, ABX_CALLER, "GetModuleHandleExA");
        if (!path.empty()) return g_getModuleHandleExW(flags, path.c_str(), module);
    }
    return g_getModuleHandleExA(flags, name, module);
}

struct Replacement {
    void* original;
    void* replacement;
};

std::vector<Replacement>& Replacements() {
    static std::vector<Replacement> replacements;
    return replacements;
}

bool TitleSide(const wchar_t* fullName, std::size_t length) {
    static const std::wstring windows = [] {
        wchar_t path[MAX_PATH];
        const UINT size = GetSystemWindowsDirectoryW(path, MAX_PATH);
        std::wstring text = size != 0 && size < MAX_PATH ? std::wstring(path, size) : std::wstring{};
        if (!text.empty() && text.back() != L'\\') text += L'\\';
        return text;
    }();
    const std::wstring path(fullName, length);
    if (!windows.empty() && path.size() >= windows.size() &&
        CompareStringOrdinal(path.c_str(), static_cast<int>(windows.size()), windows.c_str(), static_cast<int>(windows.size()), TRUE) == CSTR_EQUAL) {
        return false;
    }
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::wstring base = slash == std::wstring::npos ? path : path.substr(slash + 1);
    return !(base.size() > 3 && std::towlower(base[0]) == L'a' && std::towlower(base[1]) == L'b' && base[2] == L'_');
}

void PatchModule(void* base) {
    auto* bytes = static_cast<std::uint8_t*>(base);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(bytes + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return;
    const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0 || directory.Size == 0) return;
    for (auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(bytes + directory.VirtualAddress); descriptor->Name != 0; ++descriptor) {
        for (auto* slot = reinterpret_cast<ULONGLONG*>(bytes + descriptor->FirstThunk); *slot != 0; ++slot) {
            for (const Replacement& entry : Replacements()) {
                if (*slot != reinterpret_cast<ULONGLONG>(entry.original)) continue;
                DWORD old = 0;
                if (VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) {
                    *slot = reinterpret_cast<ULONGLONG>(entry.replacement);
                    VirtualProtect(slot, sizeof(*slot), old, &old);
                }
                break;
            }
        }
    }
}

VOID CALLBACK OnDllNotification(ULONG reason, const DllNotificationData* data, void*) {
    constexpr ULONG kLoaded = 1;
    if (reason == kLoaded && data != nullptr && data->fullName != nullptr && TitleSide(data->fullName->Buffer, data->fullName->Length / sizeof(wchar_t))) {
        PatchModule(data->base);
    }
}

void Add(HMODULE kernel32, HMODULE kernelbase, const char* name, void* replacement) {
    for (HMODULE module : {kernel32, kernelbase}) {
        if (module == nullptr) continue;
        void* original = reinterpret_cast<void*>(GetProcAddress(module, name));
        if (original != nullptr) Replacements().push_back({original, replacement});
    }
}

}

std::string RoutedLibrary(std::wstring_view requested) {
    const std::size_t slash = requested.find_last_of(L"\\/");
    std::wstring_view base = slash == std::wstring_view::npos ? requested : requested.substr(slash + 1);
    while (!base.empty() && base.back() == L' ') base.remove_suffix(1);
    std::string name;
    for (const wchar_t c : base) {
        if (c >= 128) return {};
        name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (name.empty()) return {};
    if (name.find('.') == std::string::npos) name += ".dll";
    for (const Route& route : kRoutes) {
        if (Glob(route.pattern, name)) return name == route.library ? std::string{} : route.library;
    }
    return {};
}

void InstallLoaderRedirect() {
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (kernel32 == nullptr) return;
    g_loadLibraryExW = reinterpret_cast<LoadLibraryExWFunction>(reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryExW")));
    g_loadLibraryExA = reinterpret_cast<LoadLibraryExAFunction>(reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryExA")));
    g_loadLibraryW = reinterpret_cast<LoadLibraryWFunction>(reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryW")));
    g_loadLibraryA = reinterpret_cast<LoadLibraryAFunction>(reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryA")));
    g_getModuleHandleW = reinterpret_cast<GetModuleHandleWFunction>(reinterpret_cast<void*>(GetProcAddress(kernel32, "GetModuleHandleW")));
    g_getModuleHandleA = reinterpret_cast<GetModuleHandleAFunction>(reinterpret_cast<void*>(GetProcAddress(kernel32, "GetModuleHandleA")));
    g_getModuleHandleExW = reinterpret_cast<GetModuleHandleExWFunction>(reinterpret_cast<void*>(GetProcAddress(kernel32, "GetModuleHandleExW")));
    g_getModuleHandleExA = reinterpret_cast<GetModuleHandleExAFunction>(reinterpret_cast<void*>(GetProcAddress(kernel32, "GetModuleHandleExA")));
    if (g_loadLibraryExW == nullptr || g_loadLibraryExA == nullptr || g_loadLibraryW == nullptr || g_loadLibraryA == nullptr || g_getModuleHandleW == nullptr ||
        g_getModuleHandleA == nullptr || g_getModuleHandleExW == nullptr || g_getModuleHandleExA == nullptr) {
        return;
    }
    Add(kernel32, kernelbase, "LoadLibraryExW", reinterpret_cast<void*>(&RedirectLoadLibraryExW));
    Add(kernel32, kernelbase, "LoadLibraryExA", reinterpret_cast<void*>(&RedirectLoadLibraryExA));
    Add(kernel32, kernelbase, "LoadLibraryW", reinterpret_cast<void*>(&RedirectLoadLibraryW));
    Add(kernel32, kernelbase, "LoadLibraryA", reinterpret_cast<void*>(&RedirectLoadLibraryA));
    Add(kernel32, kernelbase, "GetModuleHandleW", reinterpret_cast<void*>(&RedirectGetModuleHandleW));
    Add(kernel32, kernelbase, "GetModuleHandleA", reinterpret_cast<void*>(&RedirectGetModuleHandleA));
    Add(kernel32, kernelbase, "GetModuleHandleExW", reinterpret_cast<void*>(&RedirectGetModuleHandleExW));
    Add(kernel32, kernelbase, "GetModuleHandleExA", reinterpret_cast<void*>(&RedirectGetModuleHandleExA));

    const PEB* peb = reinterpret_cast<const PEB*>(reinterpret_cast<const TEB*>(NtCurrentTeb())->ProcessEnvironmentBlock);
    const LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;
    for (const LIST_ENTRY* link = head->Flink; link != head; link = link->Flink) {
        const auto* entry = CONTAINING_RECORD(link, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        if (entry->FullDllName.Buffer != nullptr && TitleSide(entry->FullDllName.Buffer, entry->FullDllName.Length / sizeof(wchar_t))) {
            PatchModule(entry->DllBase);
        }
    }
    const auto registerNotification = reinterpret_cast<RegisterDllNotification>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification")));
    void* cookie = nullptr;
    if (registerNotification != nullptr) registerNotification(0, &OnDllNotification, nullptr, &cookie);
}

}
