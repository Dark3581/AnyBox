#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>

namespace {

struct Fault {
    DWORD code = 0;
    std::uintptr_t offset = 0;
    wchar_t module[MAX_PATH] = L"?";
};

Fault g_fault;

LONG WINAPI RecordFault(EXCEPTION_POINTERS* pointers) {
    const EXCEPTION_RECORD* record = pointers->ExceptionRecord;
    if ((record->ExceptionCode >> 30) != 3) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_fault.code = record->ExceptionCode;
    const auto address = reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);
    HMODULE module = nullptr;
    g_fault.offset = address;
    std::wcscpy(g_fault.module, L"?");
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(record->ExceptionAddress), &module)) {
        wchar_t path[MAX_PATH];
        const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            const wchar_t* name = std::wcsrchr(path, L'\\');
            std::wcscpy(g_fault.module, name != nullptr ? name + 1 : path);
        }
        g_fault.offset = address - reinterpret_cast<std::uintptr_t>(module);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fputs("usage: anybox-module-probe <module.dll>\n", stderr);
        return 2;
    }
    AddVectoredExceptionHandler(1, RecordFault);
    const HMODULE module = LoadLibraryW(argv[1]);
    if (module == nullptr) {
        const DWORD error = GetLastError();
        if (g_fault.code != 0) {
            std::printf("failed %lu exception 0x%08lX %ls+0x%llx\n", error, g_fault.code, g_fault.module, static_cast<unsigned long long>(g_fault.offset));
        } else {
            std::printf("failed %lu\n", error);
        }
        return 1;
    }
    std::printf("loaded\n");
    return 0;
}
