#include "xdll/xgameruntime/xgameruntime.hpp"
#include "xdll/xgameruntime/TitleFolder.hpp"

#include <cstring>
#include <initializer_list>
#include <string>

DWORD abx::xgameruntime::OpenTitleFolder(const wchar_t* name, std::wstring& folder, const char* function) {
    const char* titleId = AbxTitleId();
    if (titleId == nullptr) {
        AbxUnsupported(function, "MicrosoftGame.config beside the executable has no valid <TitleId>");
    }
    wchar_t base[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        AbxUnsupported(function, "LOCALAPPDATA is not set, so there is no folder for the title's data");
    }
    std::wstring title;
    for (const char* c = titleId; *c != '\0'; ++c) {
        title += static_cast<wchar_t>(*c);
    }
    folder = std::wstring(L"\\\\?\\") + base;
    for (const wchar_t* part : {L"AnyBox", title.c_str(), name}) {
        folder += L"\\";
        folder += part;
        if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            return GetLastError();
        }
    }
    return ERROR_SUCCESS;
}

DWORD abx::xgameruntime::TitleFolderPath(const std::wstring& folder, std::string& path) {
    const std::wstring wide = folder.substr(std::wcslen(L"\\\\?\\")) + L"\\";
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size == 0) {
        return GetLastError();
    }
    path.resize(static_cast<std::size_t>(size));
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.c_str(), -1, path.data(), size, nullptr, nullptr) != size) {
        return GetLastError();
    }
    path.pop_back();
    return ERROR_SUCCESS;
}
