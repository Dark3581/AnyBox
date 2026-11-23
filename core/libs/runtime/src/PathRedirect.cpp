#include "PathRedirect.hpp"

#include "AbxApi.hpp"

#include <windows.h>

#include <array>
#include <cwctype>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace abx {

namespace {

std::wstring Widen(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string Narrow(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), length, nullptr, nullptr);
    return narrow;
}

std::wstring ExecutableFolder() {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring full(path, length);
    const std::size_t slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : full.substr(0, slash);
}

std::wstring TitleFolder() {
    wchar_t local[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    std::wstring base = length != 0 && length < MAX_PATH ? std::wstring(local, length) : ExecutableFolder();
    const char* title = AbxTitleId();
    return base + L"\\AnyBox\\" + Widen(title != nullptr ? title : "untitled");
}

void CreateFolders(const std::wstring& path) {
    for (std::size_t i = 3; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    }
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

struct Drives {
    std::array<std::optional<DriveMapping>, 26> mapping;
};

const Drives& Configured() {
    static Drives drives;
    static std::once_flag once;
    std::call_once(once, [] {
        for (wchar_t letter = L'a'; letter <= L'z'; ++letter) {
            const std::string key = std::string("runtime.drive.") + static_cast<char>(letter);
            const char* value = AbxSetting(key.c_str());
            std::optional<DriveMapping> mapping;
            if (value != nullptr) {
                std::string error;
                mapping = ParseDriveSetting(letter, value, error);
                if (!mapping) AbxUnsupported("runtime", (key + ": " + error).c_str());
            } else if (letter == L'g') {
                mapping = DriveMapping{L'g', ExecutableFolder(), true, {}};
            } else if (letter == L't') {
                mapping = DriveMapping{L't', TitleFolder() + L"\\T", false, {}};
            }
            if (!mapping) continue;
            if (mapping->readOnly && mapping->overlay.empty()) mapping->overlay = TitleFolder() + L"\\overlay\\" + std::wstring(1, static_cast<wchar_t>(std::towupper(letter)));
            if (!mapping->readOnly) CreateFolders(mapping->root);
            AbxLog("anybox", "files: %c: maps to %s%s%s", static_cast<char>(std::towupper(letter)), Narrow(mapping->root).c_str(), mapping->readOnly ? " (read-only; writes go to " : "",
                   mapping->readOnly ? (Narrow(mapping->overlay) + ")").c_str() : "");
            drives.mapping[letter - L'a'] = mapping;
        }
    });
    return drives;
}

const DriveMapping* DriveOf(std::wstring_view path) {
    if (path.size() < 2 || path[1] != L':' || !std::iswalpha(path[0])) return nullptr;
    if (path.size() > 2 && path[2] != L'\\' && path[2] != L'/') return nullptr;
    const std::optional<DriveMapping>& mapping = Configured().mapping[std::towlower(path[0]) - L'a'];
    return mapping ? &*mapping : nullptr;
}

}

std::optional<DriveMapping> ParseDriveSetting(wchar_t letter, std::string_view value, std::string& error) {
    DriveMapping mapping;
    mapping.letter = static_cast<wchar_t>(std::towlower(letter));
    std::string_view root = value;
    const std::size_t separator = value.find(';');
    if (separator != std::string_view::npos) {
        root = value.substr(0, separator);
        std::string_view option = value.substr(separator + 1);
        if (option != "readonly") {
            error = "the only drive option is ;readonly";
            return std::nullopt;
        }
        mapping.readOnly = true;
    }
    while (!root.empty() && (root.back() == '\\' || root.back() == '/')) root.remove_suffix(1);
    if (root.size() < 3 || root[1] != ':') {
        error = "a drive must map to an absolute folder";
        return std::nullopt;
    }
    mapping.root = Widen(root);
    for (wchar_t& c : mapping.root) {
        if (c == L'/') c = L'\\';
    }
    return mapping;
}

std::wstring RelativePart(std::wstring_view path) {
    std::wstring relative;
    bool separator = true;
    for (std::size_t i = 2; i < path.size(); ++i) {
        const wchar_t c = path[i] == L'/' ? L'\\' : path[i];
        if (c == L'\\') {
            if (separator) continue;
            separator = true;
        } else {
            separator = false;
        }
        relative.push_back(c);
    }
    while (!relative.empty() && relative.back() == L'\\') relative.pop_back();
    return relative;
}

std::wstring MapConsolePath(std::wstring_view path, bool write) {
    const DriveMapping* drive = DriveOf(path);
    if (drive == nullptr) return std::wstring(path);
    const std::wstring relative = RelativePart(path);
    const std::wstring original = relative.empty() ? drive->root : drive->root + L"\\" + relative;
    if (!drive->readOnly) return original;
    const std::wstring overlay = relative.empty() ? drive->overlay : drive->overlay + L"\\" + relative;
    if (write) {
        const std::size_t slash = overlay.find_last_of(L'\\');
        if (slash != std::wstring::npos) CreateFolders(overlay.substr(0, slash));
        return overlay;
    }
    return Exists(overlay) ? overlay : original;
}

namespace {

using CreateFileWFunction = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using PathFunction = BOOL(WINAPI*)(LPCWSTR);
using CreateDirectoryWFunction = BOOL(WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES);
using FindFirstFileWFunction = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
using GetDiskFreeSpaceExWFunction = BOOL(WINAPI*)(LPCWSTR, PULARGE_INTEGER, PULARGE_INTEGER, PULARGE_INTEGER);
using GetFileAttributesWFunction = DWORD(WINAPI*)(LPCWSTR);
using GetFileAttributesExWFunction = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using MoveFileExWFunction = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);
using SetFileAttributesWFunction = BOOL(WINAPI*)(LPCWSTR, DWORD);

struct Originals {
    CreateFileWFunction createFileW = nullptr;
    PathFunction deleteFileW = nullptr;
    PathFunction removeDirectoryW = nullptr;
    CreateDirectoryWFunction createDirectoryW = nullptr;
    FindFirstFileWFunction findFirstFileW = nullptr;
    GetDiskFreeSpaceExWFunction getDiskFreeSpaceExW = nullptr;
    GetFileAttributesWFunction getFileAttributesW = nullptr;
    GetFileAttributesExWFunction getFileAttributesExW = nullptr;
    MoveFileExWFunction moveFileExW = nullptr;
    SetFileAttributesWFunction setFileAttributesW = nullptr;
};

Originals g_original;

bool Tracing() {
    static const bool on = AbxTraceEnabled("files");
    return on;
}

void Trace(const char* function, LPCWSTR requested, const std::wstring& mapped, unsigned long long result) {
    if (!Tracing()) return;
    const std::string from = requested != nullptr ? Narrow(requested) : "(null)";
    const std::string to = Narrow(mapped);
    AbxLog("files", "%s(%s)%s%s = 0x%llx (error %lu)", function, from.c_str(), to == from ? "" : " -> ", to == from ? "" : to.c_str(), result, GetLastError());
}

std::wstring Mapped(LPCWSTR path, bool write) {
    return path != nullptr ? MapConsolePath(path, write) : std::wstring{};
}

LPCWSTR Pointer(LPCWSTR original, const std::wstring& mapped) {
    return original != nullptr ? mapped.c_str() : nullptr;
}

HANDLE WINAPI RedirectCreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templateFile) {
    constexpr DWORD writeAccess = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | WRITE_DAC | WRITE_OWNER | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA;
    const bool write = (access & writeAccess) != 0 || disposition == CREATE_ALWAYS || disposition == CREATE_NEW || disposition == TRUNCATE_EXISTING;
    std::wstring mapped = Mapped(path, write);
    if (write && path != nullptr && disposition != CREATE_ALWAYS && disposition != CREATE_NEW && disposition != TRUNCATE_EXISTING) {
        const std::wstring source = Mapped(path, false);
        if (source != mapped && !Exists(mapped) && Exists(source)) CopyFileW(source.c_str(), mapped.c_str(), TRUE);
    }
    const HANDLE result = g_original.createFileW(Pointer(path, mapped), access, share, security, disposition, flags, templateFile);
    Trace("CreateFileW", path, mapped, reinterpret_cast<unsigned long long>(result));
    return result;
}

BOOL WINAPI RedirectDeleteFileW(LPCWSTR path) {
    const std::wstring mapped = Mapped(path, true);
    const BOOL result = g_original.deleteFileW(Pointer(path, mapped));
    Trace("DeleteFileW", path, mapped, result);
    return result;
}

BOOL WINAPI RedirectRemoveDirectoryW(LPCWSTR path) {
    const std::wstring mapped = Mapped(path, true);
    const BOOL result = g_original.removeDirectoryW(Pointer(path, mapped));
    Trace("RemoveDirectoryW", path, mapped, result);
    return result;
}

BOOL WINAPI RedirectCreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES security) {
    const std::wstring mapped = Mapped(path, true);
    const BOOL result = g_original.createDirectoryW(Pointer(path, mapped), security);
    Trace("CreateDirectoryW", path, mapped, result);
    return result;
}

HANDLE WINAPI RedirectFindFirstFileW(LPCWSTR path, LPWIN32_FIND_DATAW data) {
    const std::wstring mapped = Mapped(path, false);
    const HANDLE result = g_original.findFirstFileW(Pointer(path, mapped), data);
    Trace("FindFirstFileW", path, mapped, reinterpret_cast<unsigned long long>(result));
    return result;
}

BOOL WINAPI RedirectGetDiskFreeSpaceExW(LPCWSTR path, PULARGE_INTEGER available, PULARGE_INTEGER total, PULARGE_INTEGER free) {
    const std::wstring mapped = Mapped(path, false);
    const BOOL result = g_original.getDiskFreeSpaceExW(Pointer(path, mapped), available, total, free);
    Trace("GetDiskFreeSpaceExW", path, mapped, result);
    return result;
}

DWORD WINAPI RedirectGetFileAttributesW(LPCWSTR path) {
    const std::wstring mapped = Mapped(path, false);
    const DWORD result = g_original.getFileAttributesW(Pointer(path, mapped));
    Trace("GetFileAttributesW", path, mapped, result);
    return result;
}

DWORD WINAPI RedirectGetFileAttributesA(LPCSTR path) {
    if (path == nullptr) return g_original.getFileAttributesW(nullptr);
    const int length = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length > 0 ? length : 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide.data(), length);
    return RedirectGetFileAttributesW(wide.c_str());
}

BOOL WINAPI RedirectGetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID information) {
    const std::wstring mapped = Mapped(path, false);
    const BOOL result = g_original.getFileAttributesExW(Pointer(path, mapped), level, information);
    Trace("GetFileAttributesExW", path, mapped, result);
    return result;
}

BOOL WINAPI RedirectMoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
    const std::wstring source = Mapped(from, true);
    const std::wstring target = Mapped(to, true);
    const BOOL result = g_original.moveFileExW(Pointer(from, source), Pointer(to, target), flags);
    Trace("MoveFileExW", from, source, result);
    return result;
}

BOOL WINAPI RedirectSetFileAttributesW(LPCWSTR path, DWORD attributes) {
    const std::wstring mapped = Mapped(path, true);
    const BOOL result = g_original.setFileAttributesW(Pointer(path, mapped), attributes);
    Trace("SetFileAttributesW", path, mapped, result);
    return result;
}

template <typename Function>
bool Load(HMODULE module, const char* name, Function& target) {
    target = reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(module, name)));
    return target != nullptr;
}

}

void AddPathRedirects(const std::function<void(const char*, void*)>& add) {
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32 == nullptr) return;
    const bool loaded = Load(kernel32, "CreateFileW", g_original.createFileW) && Load(kernel32, "DeleteFileW", g_original.deleteFileW) &&
                        Load(kernel32, "RemoveDirectoryW", g_original.removeDirectoryW) && Load(kernel32, "CreateDirectoryW", g_original.createDirectoryW) &&
                        Load(kernel32, "FindFirstFileW", g_original.findFirstFileW) && Load(kernel32, "GetDiskFreeSpaceExW", g_original.getDiskFreeSpaceExW) &&
                        Load(kernel32, "GetFileAttributesW", g_original.getFileAttributesW) && Load(kernel32, "GetFileAttributesExW", g_original.getFileAttributesExW) &&
                        Load(kernel32, "MoveFileExW", g_original.moveFileExW) && Load(kernel32, "SetFileAttributesW", g_original.setFileAttributesW);
    if (!loaded) return;
    add("CreateFileW", reinterpret_cast<void*>(&RedirectCreateFileW));
    add("DeleteFileW", reinterpret_cast<void*>(&RedirectDeleteFileW));
    add("RemoveDirectoryW", reinterpret_cast<void*>(&RedirectRemoveDirectoryW));
    add("CreateDirectoryW", reinterpret_cast<void*>(&RedirectCreateDirectoryW));
    add("FindFirstFileW", reinterpret_cast<void*>(&RedirectFindFirstFileW));
    add("GetDiskFreeSpaceExW", reinterpret_cast<void*>(&RedirectGetDiskFreeSpaceExW));
    add("GetFileAttributesW", reinterpret_cast<void*>(&RedirectGetFileAttributesW));
    add("GetFileAttributesA", reinterpret_cast<void*>(&RedirectGetFileAttributesA));
    add("GetFileAttributesExW", reinterpret_cast<void*>(&RedirectGetFileAttributesExW));
    add("MoveFileExW", reinterpret_cast<void*>(&RedirectMoveFileExW));
    add("SetFileAttributesW", reinterpret_cast<void*>(&RedirectSetFileAttributesW));
}

}

ABX_RUNTIME_API std::size_t AbxMapConsolePath(const wchar_t* path, bool write, wchar_t* buffer, std::size_t capacity) {
    if (path == nullptr) return 0;
    const std::wstring mapped = abx::MapConsolePath(path, write);
    if (buffer != nullptr && capacity > mapped.size()) {
        mapped.copy(buffer, mapped.size());
        buffer[mapped.size()] = L'\0';
    }
    return mapped.size() + 1;
}
