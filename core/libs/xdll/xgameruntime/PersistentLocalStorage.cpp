#include "xdll/xgameruntime/xgameruntime.hpp"
#include "xdll/xgameruntime/TitleFolder.hpp"

#include <cstdint>
#include <cstring>
#include <string>

namespace {

HRESULT StoragePath(std::wstring& folder, std::string& path, const char* function) {
    DWORD error = abx::xgameruntime::OpenTitleFolder(L"PersistentLocalStorage", folder, function);
    if (error == ERROR_SUCCESS) {
        error = abx::xgameruntime::TitleFolderPath(folder, path);
    }
    return HRESULT_FROM_WIN32(error);
}

HRESULT FolderBytes(const std::wstring& folder, std::uint64_t& bytes, const char* function) {
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(error);
    }
    HRESULT result = S_OK;
    do {
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            bytes += (static_cast<std::uint64_t>(found.nFileSizeHigh) << 32) | found.nFileSizeLow;
            continue;
        }
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            AbxUnsupported(function, "a folder link (reparse point) inside the persistent local storage folder");
        }
        result = FolderBytes(folder + L"\\" + name, bytes, function);
    } while (SUCCEEDED(result) && FindNextFileW(search, &found));
    FindClose(search);
    return result;
}

}

ABX_API HRESULT XPersistentLocalStorageGetPathSize(std::size_t* pathSize) {
    if (pathSize == nullptr) {
        AbxUnsupported(__func__, "pathSize is null");
    }
    std::wstring folder;
    std::string path;
    const HRESULT result = StoragePath(folder, path, __func__);
    if (FAILED(result)) {
        return result;
    }
    *pathSize = path.size() + 1;
    return S_OK;
}

ABX_API HRESULT XPersistentLocalStorageGetPath(std::size_t pathSize, char* path, std::size_t* pathUsed) {
    std::wstring folder;
    std::string storage;
    const HRESULT result = StoragePath(folder, storage, __func__);
    if (FAILED(result)) {
        return result;
    }
    if (pathSize < storage.size() + 1) {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    if (path == nullptr) {
        AbxUnsupported(__func__, "path is null");
    }
    std::memcpy(path, storage.c_str(), storage.size() + 1);
    if (pathUsed != nullptr) {
        *pathUsed = storage.size() + 1;
    }
    return S_OK;
}

ABX_API HRESULT XPersistentLocalStorageGetSpaceInfo(XPersistentLocalStorageSpaceInfo* info) {
    if (info == nullptr) {
        AbxUnsupported(__func__, "info is null");
    }
    std::wstring folder;
    std::string path;
    const HRESULT result = StoragePath(folder, path, __func__);
    if (FAILED(result)) {
        return result;
    }
    std::uint64_t used = 0;
    const HRESULT measured = FolderBytes(folder, used, __func__);
    if (FAILED(measured)) {
        return measured;
    }
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW((folder + L"\\").c_str(), &available, nullptr, nullptr)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    info->availableFreeBytes = available.QuadPart;
    info->totalFreeBytes = available.QuadPart;
    info->usedBytes = used;
    info->totalBytes = used + available.QuadPart;
    return S_OK;
}

ABX_API HRESULT XPersistentLocalStoragePromptUserForSpaceAsync(std::uint64_t, XAsyncBlock*) {
    AbxUnsupported(__func__, "there is no system UI to ask the user to free space, and what the call completes with on PC is not documented");
}

ABX_API HRESULT XPersistentLocalStoragePromptUserForSpaceResult(XAsyncBlock*) {
    AbxUnsupported(__func__, "no prompt for space can have started, because XPersistentLocalStoragePromptUserForSpaceAsync always stops");
}
