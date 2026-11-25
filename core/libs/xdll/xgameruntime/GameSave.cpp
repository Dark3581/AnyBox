#include "xdll/xgameruntime/xgameruntime.hpp"
#include "xdll/xgameruntime/TitleFolder.hpp"

#include <atomic>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

struct XGameSaveProvider {
    std::wstring root;
};

struct XGameSaveContainer {
    std::wstring root;
    std::string name;
};

struct XGameSaveUpdate {
    struct Change {
        std::string name;
        bool remove = false;
        std::vector<std::uint8_t> data;
    };

    std::wstring root;
    std::string name;
    std::string displayName;
    bool keepDisplayName = false;
    std::vector<Change> changes;
    std::atomic<bool> submitted{false};
};

namespace {

constexpr HRESULT InvalidContainerName = static_cast<HRESULT>(0x80830001u);
constexpr HRESULT OutOfLocalStorage = static_cast<HRESULT>(0x80830003u);
constexpr HRESULT UpdateTooBig = static_cast<HRESULT>(0x80830005u);
constexpr HRESULT QuotaExceeded = static_cast<HRESULT>(0x80830006u);
constexpr HRESULT ProvidedBufferTooSmall = static_cast<HRESULT>(0x80830007u);
constexpr HRESULT BlobNotFound = static_cast<HRESULT>(0x80830008u);
constexpr HRESULT HandleExpired = static_cast<HRESULT>(0x8083000Du);
constexpr HRESULT ProviderMismatch = static_cast<HRESULT>(0x8083000Fu);
constexpr HRESULT NotSufficientBuffer = static_cast<HRESULT>(0x8007007Au);

constexpr std::size_t MaxContainerName = 256;
constexpr std::uint64_t MaxUpdateSize = 16ull * 1024 * 1024;
constexpr std::int64_t Quota = 256ll * 1024 * 1024;
constexpr std::uint32_t FileMagic = 0x53475841;
constexpr std::uint32_t FileVersion = 1;
constexpr const wchar_t* Extension = L".xgs";

struct Blob {
    std::string name;
    std::vector<std::uint8_t> data;
};

struct Stored {
    std::string name;
    std::string displayName;
    std::vector<Blob> blobs;
    std::time_t modified = 0;
};

std::mutex& StorageMutex() {
    static std::mutex* mutex = new std::mutex();
    return *mutex;
}

enum class SaveSystem { None, GameSave, GameSaveFiles };

std::atomic<SaveSystem> g_saveSystem{SaveSystem::None};

bool Available(SaveSystem system) {
    const SaveSystem current = g_saveSystem.load();
    return current == SaveSystem::None || current == system;
}

bool Claim(SaveSystem system) {
    SaveSystem expected = SaveSystem::None;
    return g_saveSystem.compare_exchange_strong(expected, system) || expected == system;
}

bool IsAlnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

char Lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool SameName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (Lower(a[i]) != Lower(b[i])) {
            return false;
        }
    }
    return true;
}

bool HasPrefix(const std::string& name, const char* prefix) {
    if (prefix == nullptr) {
        return true;
    }
    const std::size_t length = std::strlen(prefix);
    return name.size() >= length && SameName(name.substr(0, length), prefix);
}

bool ValidContainerName(const char* name) {
    if (name == nullptr) {
        return false;
    }
    const std::string text = name;
    if (text.empty() || text.size() > MaxContainerName) {
        return false;
    }
    const std::size_t slash = text.rfind('/');
    const std::size_t fileStart = slash == std::string::npos ? 0 : slash + 1;
    for (std::size_t i = 0; i < fileStart; ++i) {
        if (!IsAlnum(text[i]) && text[i] != '_' && text[i] != '/') {
            return false;
        }
    }
    if (fileStart == text.size() || text.back() == '.' || text.find("..", fileStart) != std::string::npos) {
        return false;
    }
    for (std::size_t i = fileStart; i < text.size(); ++i) {
        if (!IsAlnum(text[i]) && text[i] != '_' && text[i] != '.' && text[i] != '-') {
            return false;
        }
    }
    return true;
}

std::wstring ContainerPath(const std::wstring& root, const std::string& name, const char* function) {
    std::wstring stem;
    for (const char c : name) {
        stem += c == '/' ? L'%' : static_cast<wchar_t>(c);
    }
    if (stem.size() + std::wcslen(Extension) > 255) {
        AbxUnsupported(function, "container name longer than 251 characters, the host file name limit");
    }
    return root + L"\\" + stem + Extension;
}

HRESULT FromWin32(DWORD error) {
    if (error == ERROR_DISK_FULL || error == ERROR_HANDLE_DISK_FULL) {
        return OutOfLocalStorage;
    }
    return HRESULT_FROM_WIN32(error);
}

std::time_t ToTime(const FILETIME& time) {
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return static_cast<std::time_t>((value.QuadPart - 116444736000000000ull) / 10000000ull);
}

class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    bool Read(void* out, std::size_t size) {
        if (bytes_.size() - offset_ < size) {
            return false;
        }
        std::memcpy(out, bytes_.data() + offset_, size);
        offset_ += size;
        return true;
    }

    bool U32(std::uint32_t& value) {
        return Read(&value, sizeof(value));
    }

    bool Text(std::string& text) {
        std::uint32_t size = 0;
        if (!U32(size) || bytes_.size() - offset_ < size) {
            return false;
        }
        text.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
        offset_ += size;
        return true;
    }

    bool Bytes(std::vector<std::uint8_t>& data) {
        std::uint32_t size = 0;
        if (!U32(size) || bytes_.size() - offset_ < size) {
            return false;
        }
        data.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_), bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
        offset_ += size;
        return true;
    }

    bool AtEnd() const {
        return offset_ == bytes_.size();
    }

private:
    const std::vector<std::uint8_t>& bytes_;
    std::size_t offset_ = 0;
};

void PutU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(value));
}

void PutBytes(std::vector<std::uint8_t>& out, const void* data, std::size_t size) {
    PutU32(out, static_cast<std::uint32_t>(size));
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

HRESULT Load(const std::wstring& path, Stored& stored, bool& exists, const char* function) {
    exists = false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? S_OK : FromWin32(error);
    }
    LARGE_INTEGER size{};
    FILETIME written{};
    std::vector<std::uint8_t> bytes;
    bool read = GetFileSizeEx(file, &size) && GetFileTime(file, nullptr, nullptr, &written) && size.QuadPart <= 0x7fffffff;
    if (read) {
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD done = 0;
        read = bytes.empty() || (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &done, nullptr) && done == bytes.size());
    }
    const DWORD error = GetLastError();
    CloseHandle(file);
    if (!read) {
        return FromWin32(error);
    }
    Reader reader(bytes);
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t count = 0;
    bool valid = reader.U32(magic) && magic == FileMagic && reader.U32(version) && version == FileVersion && reader.Text(stored.name) && reader.Text(stored.displayName) && reader.U32(count);
    stored.blobs.clear();
    for (std::uint32_t i = 0; valid && i < count; ++i) {
        Blob blob;
        valid = reader.Text(blob.name) && reader.Bytes(blob.data);
        stored.blobs.push_back(std::move(blob));
    }
    if (!valid || !reader.AtEnd()) {
        AbxUnsupported(function, "a file in the XGameSave folder is not a save container written by AnyBox");
    }
    stored.modified = ToTime(written);
    exists = true;
    return S_OK;
}

HRESULT Store(const std::wstring& root, const std::wstring& path, const Stored& stored) {
    std::vector<std::uint8_t> bytes;
    PutU32(bytes, FileMagic);
    PutU32(bytes, FileVersion);
    PutBytes(bytes, stored.name.data(), stored.name.size());
    PutBytes(bytes, stored.displayName.data(), stored.displayName.size());
    PutU32(bytes, static_cast<std::uint32_t>(stored.blobs.size()));
    for (const Blob& blob : stored.blobs) {
        PutBytes(bytes, blob.name.data(), blob.name.size());
        PutBytes(bytes, blob.data.data(), blob.data.size());
    }
    const std::wstring temporary = root + L"\\update.tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return FromWin32(GetLastError());
    }
    DWORD done = 0;
    const bool written = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &done, nullptr) && done == bytes.size() && FlushFileBuffers(file);
    const DWORD error = GetLastError();
    CloseHandle(file);
    if (!written) {
        DeleteFileW(temporary.c_str());
        return FromWin32(error);
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD moveError = GetLastError();
        DeleteFileW(temporary.c_str());
        return FromWin32(moveError);
    }
    return S_OK;
}

HRESULT ForEachContainer(const std::wstring& root, const std::function<HRESULT(const std::wstring&)>& visit) {
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((root + L"\\*" + Extension).c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? S_OK : FromWin32(error);
    }
    HRESULT result = S_OK;
    do {
        const std::wstring file = found.cFileName;
        const std::size_t extension = std::wcslen(Extension);
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || file.size() <= extension || file.compare(file.size() - extension, extension, Extension) != 0) {
            continue;
        }
        result = visit(root + L"\\" + file);
    } while (SUCCEEDED(result) && FindNextFileW(search, &found));
    FindClose(search);
    return result;
}

HRESULT LoadAll(const std::wstring& root, std::vector<Stored>& all, const char* function) {
    return ForEachContainer(root, [&](const std::wstring& path) {
        Stored stored;
        bool exists = false;
        const HRESULT result = Load(path, stored, exists, function);
        if (exists) {
            all.push_back(std::move(stored));
        }
        return result;
    });
}

HRESULT StoredBlobBytes(const std::wstring& path, std::uint64_t& total, const char* function) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? S_OK : FromWin32(error);
    }
    LARGE_INTEGER size{};
    std::uint64_t offset = 0;
    DWORD error = ERROR_SUCCESS;
    const auto u32 = [&](std::uint32_t& value) {
        DWORD done = 0;
        if (!ReadFile(file, &value, sizeof(value), &done, nullptr)) {
            error = GetLastError();
            return false;
        }
        offset += done;
        return done == sizeof(value);
    };
    const auto skip = [&](std::uint32_t count) {
        LARGE_INTEGER move{};
        move.QuadPart = count;
        if (!SetFilePointerEx(file, move, nullptr, FILE_CURRENT)) {
            error = GetLastError();
            return false;
        }
        offset += count;
        return offset <= static_cast<std::uint64_t>(size.QuadPart);
    };
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t length = 0;
    std::uint32_t count = 0;
    std::uint64_t bytes = 0;
    if (!GetFileSizeEx(file, &size)) {
        error = GetLastError();
    }
    bool valid = error == ERROR_SUCCESS && u32(magic) && magic == FileMagic && u32(version) && version == FileVersion && u32(length) && skip(length) && u32(length) && skip(length) && u32(count);
    for (std::uint32_t i = 0; valid && i < count; ++i) {
        valid = u32(length) && skip(length) && u32(length) && skip(length);
        bytes += length;
    }
    CloseHandle(file);
    if (error != ERROR_SUCCESS) {
        return FromWin32(error);
    }
    if (!valid || offset != static_cast<std::uint64_t>(size.QuadPart)) {
        AbxUnsupported(function, "a file in the XGameSave folder is not a save container written by AnyBox");
    }
    total += bytes;
    return S_OK;
}

std::uint64_t BlobBytes(const Stored& stored) {
    std::uint64_t total = 0;
    for (const Blob& blob : stored.blobs) {
        total += blob.data.size();
    }
    return total;
}

HRESULT ForEachEntry(const std::wstring& folder, const std::function<HRESULT(const std::wstring&, const WIN32_FIND_DATAW&)>& visit, const char* function) {
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? S_OK : FromWin32(error);
    }
    HRESULT result = S_OK;
    do {
        const std::wstring name = found.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            AbxUnsupported(function, "a folder link (reparse point) inside the save folder");
        }
        result = visit(folder + L"\\" + name, found);
    } while (SUCCEEDED(result) && FindNextFileW(search, &found));
    FindClose(search);
    return result;
}

bool IsFolder(const WIN32_FIND_DATAW& found) {
    return (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

HRESULT FolderContents(const std::wstring& folder, std::uint64_t& bytes, std::uint64_t& files, const char* function) {
    return ForEachEntry(folder, [&](const std::wstring& path, const WIN32_FIND_DATAW& found) {
        if (IsFolder(found)) {
            return FolderContents(path, bytes, files, function);
        }
        bytes += (static_cast<std::uint64_t>(found.nFileSizeHigh) << 32) | found.nFileSizeLow;
        ++files;
        return S_OK;
    }, function);
}

HRESULT SubfolderContents(const std::wstring& root, std::uint64_t& bytes, std::uint64_t& files, const char* function) {
    return ForEachEntry(root, [&](const std::wstring& path, const WIN32_FIND_DATAW& found) {
        return IsFolder(found) ? FolderContents(path, bytes, files, function) : S_OK;
    }, function);
}

HRESULT HasContainers(const std::wstring& root, bool& any) {
    any = false;
    return ForEachContainer(root, [&](const std::wstring&) {
        any = true;
        return S_OK;
    });
}

HRESULT Used(const std::wstring& root, std::int64_t& used, const char* function) {
    std::uint64_t total = 0;
    HRESULT result = ForEachContainer(root, [&](const std::wstring& path) { return StoredBlobBytes(path, total, function); });
    std::uint64_t files = 0;
    if (SUCCEEDED(result)) {
        result = SubfolderContents(root, total, files, function);
    }
    used = static_cast<std::int64_t>(total);
    return result;
}

HRESULT OpenRoot(std::wstring& root, const char* function) {
    const DWORD error = abx::xgameruntime::OpenTitleFolder(L"XGameSave", root, function);
    return error == ERROR_SUCCESS ? S_OK : FromWin32(error);
}

HRESULT InitializeProvider(const char* configurationId, std::unique_ptr<XGameSaveProvider>& provider, const char* function) {
    if (configurationId == nullptr) {
        return E_INVALIDARG;
    }
    if (!Available(SaveSystem::GameSave)) {
        return ProviderMismatch;
    }
    std::wstring root;
    const HRESULT opened = OpenRoot(root, function);
    if (FAILED(opened)) {
        return opened;
    }
    std::uint64_t bytes = 0;
    std::uint64_t files = 0;
    {
        std::lock_guard lock(StorageMutex());
        const HRESULT listed = SubfolderContents(root, bytes, files, function);
        if (FAILED(listed)) {
            return listed;
        }
    }
    if (files != 0) {
        AbxUnsupported(function, "the save folder holds XGameSaveFiles files, and presenting them as XGameSave containers is not implemented");
    }
    provider.reset(new (std::nothrow) XGameSaveProvider{std::move(root)});
    if (provider == nullptr) {
        return E_OUTOFMEMORY;
    }
    if (!Claim(SaveSystem::GameSave)) {
        provider.reset();
        return ProviderMismatch;
    }
    return S_OK;
}

HRESULT RemainingQuota(const std::wstring& root, std::int64_t& remaining, const char* function) {
    std::lock_guard lock(StorageMutex());
    std::int64_t used = 0;
    const HRESULT result = Used(root, used, function);
    remaining = used >= Quota ? 0 : Quota - used;
    return result;
}

HRESULT SaveFilesFolder(std::string& folder, const char* function) {
    if (!Available(SaveSystem::GameSaveFiles)) {
        return ProviderMismatch;
    }
    std::wstring root;
    const HRESULT opened = OpenRoot(root, function);
    if (FAILED(opened)) {
        return opened;
    }
    bool containers = false;
    {
        std::lock_guard lock(StorageMutex());
        const HRESULT listed = HasContainers(root, containers);
        if (FAILED(listed)) {
            return listed;
        }
    }
    if (containers) {
        AbxUnsupported(function, "the save folder holds XGameSave containers, and presenting them as XGameSaveFiles folders and files is not implemented");
    }
    const DWORD error = abx::xgameruntime::TitleFolderPath(root, folder);
    if (error != ERROR_SUCCESS) {
        return FromWin32(error);
    }
    return Claim(SaveSystem::GameSaveFiles) ? S_OK : ProviderMismatch;
}

HRESULT DeleteContainer(const std::wstring& root, const std::string& name, const char* function) {
    const std::wstring path = ContainerPath(root, name, function);
    std::lock_guard lock(StorageMutex());
    if (!DeleteFileW(path.c_str())) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? S_OK : FromWin32(error);
    }
    return S_OK;
}

void Lay(const std::vector<Blob>& blobs, void* buffer) {
    auto* entries = static_cast<XGameSaveBlob*>(buffer);
    auto* cursor = reinterpret_cast<std::uint8_t*>(entries + blobs.size());
    for (std::size_t i = 0; i < blobs.size(); ++i) {
        std::memcpy(cursor, blobs[i].name.c_str(), blobs[i].name.size() + 1);
        entries[i].info.name = reinterpret_cast<const char*>(cursor);
        entries[i].info.size = static_cast<std::uint32_t>(blobs[i].data.size());
        cursor += blobs[i].name.size() + 1;
        if (!blobs[i].data.empty()) {
            std::memcpy(cursor, blobs[i].data.data(), blobs[i].data.size());
        }
        entries[i].data = cursor;
        cursor += blobs[i].data.size();
    }
}

std::size_t LaidSize(const std::vector<Blob>& blobs) {
    std::size_t size = 0;
    for (const Blob& blob : blobs) {
        size += sizeof(XGameSaveBlob) + blob.name.size() + 1 + blob.data.size();
    }
    return size;
}

HRESULT ReadBlobs(XGameSaveContainerHandle container, const std::vector<std::string>& names, bool all, std::vector<Blob>& blobs, const char* function) {
    Stored stored;
    bool exists = false;
    {
        std::lock_guard lock(StorageMutex());
        const HRESULT loaded = Load(ContainerPath(container->root, container->name, function), stored, exists, function);
        if (FAILED(loaded)) {
            return loaded;
        }
    }
    if (all) {
        blobs = std::move(stored.blobs);
        return S_OK;
    }
    for (const std::string& name : names) {
        bool found = false;
        for (Blob& blob : stored.blobs) {
            if (SameName(blob.name, name)) {
                blobs.push_back(blob);
                found = true;
                break;
            }
        }
        if (!found) {
            return BlobNotFound;
        }
    }
    return S_OK;
}

HRESULT SubmitUpdate(XGameSaveUpdateHandle update, const char* function) {
    std::uint64_t updateSize = 0;
    for (const XGameSaveUpdate::Change& change : update->changes) {
        updateSize += change.data.size();
    }
    if (updateSize > MaxUpdateSize) {
        return UpdateTooBig;
    }
    const std::wstring path = ContainerPath(update->root, update->name, function);
    std::lock_guard lock(StorageMutex());
    Stored stored;
    bool exists = false;
    HRESULT result = Load(path, stored, exists, function);
    if (FAILED(result)) {
        return result;
    }
    const std::uint64_t before = exists ? BlobBytes(stored) : 0;
    stored.name = update->name;
    if (!update->keepDisplayName || !exists) {
        stored.displayName = update->displayName;
    }
    for (const XGameSaveUpdate::Change& change : update->changes) {
        std::size_t index = 0;
        while (index < stored.blobs.size() && !SameName(stored.blobs[index].name, change.name)) {
            ++index;
        }
        if (change.remove) {
            if (index < stored.blobs.size()) {
                stored.blobs.erase(stored.blobs.begin() + static_cast<std::ptrdiff_t>(index));
            }
        } else if (index < stored.blobs.size()) {
            stored.blobs[index] = {change.name, change.data};
        } else {
            stored.blobs.push_back({change.name, change.data});
        }
    }
    std::int64_t used = 0;
    result = Used(update->root, used, function);
    if (FAILED(result)) {
        return result;
    }
    if (used - static_cast<std::int64_t>(before) + static_cast<std::int64_t>(BlobBytes(stored)) > Quota) {
        return QuotaExceeded;
    }
    return Store(update->root, path, stored);
}

HRESULT ContainerInfo(XGameSaveProviderHandle provider, const char* name, const char* prefix, void* context, XGameSaveContainerInfoCallback* callback, const char* function) {
    std::vector<Stored> all;
    {
        std::lock_guard lock(StorageMutex());
        HRESULT result = S_OK;
        if (name != nullptr) {
            Stored stored;
            bool exists = false;
            result = Load(ContainerPath(provider->root, name, function), stored, exists, function);
            if (exists) {
                all.push_back(std::move(stored));
            }
        } else {
            result = LoadAll(provider->root, all, function);
        }
        if (FAILED(result)) {
            return result;
        }
    }
    for (const Stored& stored : all) {
        if (!HasPrefix(stored.name, prefix)) {
            continue;
        }
        const XGameSaveContainerInfo info{stored.name.c_str(), stored.displayName.c_str(), static_cast<std::uint32_t>(stored.blobs.size()), BlobBytes(stored), stored.modified, false};
        if (!callback(&info, context)) {
            break;
        }
    }
    return S_OK;
}

HRESULT BlobInfo(XGameSaveContainerHandle container, const char* prefix, void* context, XGameSaveBlobInfoCallback* callback, const char* function) {
    if (container == nullptr || callback == nullptr) {
        return E_INVALIDARG;
    }
    std::vector<Blob> blobs;
    const HRESULT result = ReadBlobs(container, {}, true, blobs, function);
    if (FAILED(result)) {
        return result;
    }
    for (const Blob& blob : blobs) {
        if (!HasPrefix(blob.name, prefix)) {
            continue;
        }
        const XGameSaveBlobInfo info{blob.name.c_str(), static_cast<std::uint32_t>(blob.data.size())};
        if (!callback(&info, context)) {
            break;
        }
    }
    return S_OK;
}

HRESULT AddChange(XGameSaveUpdateHandle update, const char* blobName, bool remove, const std::uint8_t* data, std::size_t byteCount, const char* function) {
    if (update == nullptr || blobName == nullptr || *blobName == '\0' || (!remove && data == nullptr && byteCount != 0)) {
        return E_INVALIDARG;
    }
    if (update->submitted) {
        return HandleExpired;
    }
    if (byteCount > 0xffffffffu) {
        AbxUnsupported(function, "a blob larger than 4 GiB does not fit XGameSaveBlobInfo::size");
    }
    for (const XGameSaveUpdate::Change& change : update->changes) {
        if (SameName(change.name, blobName)) {
            return E_INVALIDARG;
        }
    }
    XGameSaveUpdate::Change change;
    change.name = blobName;
    change.remove = remove;
    if (!remove && byteCount != 0) {
        change.data.assign(data, data + byteCount);
    }
    update->changes.push_back(std::move(change));
    return S_OK;
}

struct Operation {
    std::function<HRESULT(Operation&)> work;
    std::size_t size = 0;
    std::function<void(void*)> deliver;
    std::unique_ptr<XGameSaveProvider> provider;
    std::vector<Blob> blobs;
};

HRESULT OperationProvider(XAsyncOp op, const XAsyncProviderData* data) {
    auto* operation = static_cast<Operation*>(data->context);
    switch (op) {
    case XAsyncOp::Begin:
        return XAsyncSchedule(data->async, 0);
    case XAsyncOp::DoWork: {
        const HRESULT result = operation->work(*operation);
        XAsyncComplete(data->async, result, SUCCEEDED(result) ? operation->size : 0);
        return S_OK;
    }
    case XAsyncOp::GetResult:
        if (operation->deliver) {
            operation->deliver(data->buffer);
        }
        return S_OK;
    case XAsyncOp::Cleanup:
        delete operation;
        return S_OK;
    default:
        return S_OK;
    }
}

HRESULT Start(XAsyncBlock* async, const void* identity, const char* function, std::function<HRESULT(Operation&)> work) {
    if (async == nullptr) {
        return E_INVALIDARG;
    }
    Operation* operation = new (std::nothrow) Operation();
    if (operation == nullptr) {
        return E_OUTOFMEMORY;
    }
    operation->work = std::move(work);
    const HRESULT begun = XAsyncBegin(async, operation, identity, function, OperationProvider);
    if (FAILED(begun)) {
        delete operation;
    }
    return begun;
}

}

ABX_API HRESULT XGameSaveInitializeProvider(XUserHandle, const char* configurationId, bool, XGameSaveProviderHandle* provider) {
    if (provider == nullptr) {
        return E_INVALIDARG;
    }
    std::unique_ptr<XGameSaveProvider> created;
    const HRESULT result = InitializeProvider(configurationId, created, __func__);
    *provider = created.release();
    return result;
}

ABX_API HRESULT XGameSaveInitializeProviderAsync(XUserHandle, const char* configurationId, bool, XAsyncBlock* async) {
    if (configurationId == nullptr) {
        return E_INVALIDARG;
    }
    const std::string id = configurationId;
    return Start(async, reinterpret_cast<const void*>(&XGameSaveInitializeProviderAsync), __func__, [id](Operation& operation) {
        const HRESULT result = InitializeProvider(id.c_str(), operation.provider, "XGameSaveInitializeProviderAsync");
        operation.size = sizeof(XGameSaveProviderHandle);
        operation.deliver = [&operation](void* buffer) {
            XGameSaveProviderHandle handle = operation.provider.release();
            std::memcpy(buffer, &handle, sizeof(handle));
        };
        return result;
    });
}

ABX_API HRESULT XGameSaveInitializeProviderResult(XAsyncBlock* async, XGameSaveProviderHandle* provider) {
    if (provider == nullptr) {
        return E_INVALIDARG;
    }
    return XAsyncGetResult(async, reinterpret_cast<const void*>(&XGameSaveInitializeProviderAsync), sizeof(*provider), provider, nullptr);
}

ABX_API void XGameSaveCloseProvider(XGameSaveProviderHandle provider) {
    delete provider;
}

ABX_API HRESULT XGameSaveGetRemainingQuota(XGameSaveProviderHandle provider, std::int64_t* remainingQuota) {
    if (provider == nullptr || remainingQuota == nullptr) {
        return E_INVALIDARG;
    }
    return RemainingQuota(provider->root, *remainingQuota, __func__);
}

ABX_API HRESULT XGameSaveGetRemainingQuotaAsync(XGameSaveProviderHandle provider, XAsyncBlock* async) {
    if (provider == nullptr) {
        return E_INVALIDARG;
    }
    return Start(async, reinterpret_cast<const void*>(&XGameSaveGetRemainingQuotaAsync), __func__, [provider](Operation& operation) {
        auto remaining = std::make_shared<std::int64_t>(0);
        const HRESULT result = RemainingQuota(provider->root, *remaining, "XGameSaveGetRemainingQuotaAsync");
        operation.size = sizeof(std::int64_t);
        operation.deliver = [remaining](void* buffer) { std::memcpy(buffer, remaining.get(), sizeof(std::int64_t)); };
        return result;
    });
}

ABX_API HRESULT XGameSaveGetRemainingQuotaResult(XAsyncBlock* async, std::int64_t* remainingQuota) {
    if (remainingQuota == nullptr) {
        return E_INVALIDARG;
    }
    return XAsyncGetResult(async, reinterpret_cast<const void*>(&XGameSaveGetRemainingQuotaAsync), sizeof(*remainingQuota), remainingQuota, nullptr);
}

ABX_API HRESULT XGameSaveDeleteContainer(XGameSaveProviderHandle provider, const char* containerName) {
    if (provider == nullptr) {
        return E_INVALIDARG;
    }
    if (!ValidContainerName(containerName)) {
        return InvalidContainerName;
    }
    return DeleteContainer(provider->root, containerName, __func__);
}

ABX_API HRESULT XGameSaveDeleteContainerAsync(XGameSaveProviderHandle provider, const char* containerName, XAsyncBlock* async) {
    if (provider == nullptr) {
        return E_INVALIDARG;
    }
    if (!ValidContainerName(containerName)) {
        return InvalidContainerName;
    }
    const std::wstring root = provider->root;
    const std::string name = containerName;
    return Start(async, reinterpret_cast<const void*>(&XGameSaveDeleteContainerAsync), __func__, [root, name](Operation&) {
        return DeleteContainer(root, name, "XGameSaveDeleteContainerAsync");
    });
}

ABX_API HRESULT XGameSaveDeleteContainerResult(XAsyncBlock* async) {
    return XAsyncGetResult(async, reinterpret_cast<const void*>(&XGameSaveDeleteContainerAsync), 0, nullptr, nullptr);
}

ABX_API HRESULT XGameSaveCreateContainer(XGameSaveProviderHandle provider, const char* containerName, XGameSaveContainerHandle* containerContext) {
    if (provider == nullptr || containerContext == nullptr) {
        return E_INVALIDARG;
    }
    if (!ValidContainerName(containerName)) {
        return InvalidContainerName;
    }
    ContainerPath(provider->root, containerName, __func__);
    *containerContext = new (std::nothrow) XGameSaveContainer{provider->root, containerName};
    return *containerContext == nullptr ? E_OUTOFMEMORY : S_OK;
}

ABX_API void XGameSaveCloseContainer(XGameSaveContainerHandle context) {
    delete context;
}

ABX_API HRESULT XGameSaveGetContainerInfo(XGameSaveProviderHandle provider, const char* containerName, void* context, XGameSaveContainerInfoCallback* callback) {
    if (provider == nullptr || callback == nullptr) {
        return E_INVALIDARG;
    }
    if (!ValidContainerName(containerName)) {
        return InvalidContainerName;
    }
    return ContainerInfo(provider, containerName, nullptr, context, callback, __func__);
}

ABX_API HRESULT XGameSaveEnumerateContainerInfo(XGameSaveProviderHandle provider, void* context, XGameSaveContainerInfoCallback* callback) {
    if (provider == nullptr || callback == nullptr) {
        return E_INVALIDARG;
    }
    return ContainerInfo(provider, nullptr, nullptr, context, callback, __func__);
}

ABX_API HRESULT XGameSaveEnumerateContainerInfoByName(XGameSaveProviderHandle provider, const char* containerNamePrefix, void* context, XGameSaveContainerInfoCallback* callback) {
    if (provider == nullptr || callback == nullptr) {
        return E_INVALIDARG;
    }
    return ContainerInfo(provider, nullptr, containerNamePrefix, context, callback, __func__);
}

ABX_API HRESULT XGameSaveEnumerateBlobInfo(XGameSaveContainerHandle container, void* context, XGameSaveBlobInfoCallback* callback) {
    return BlobInfo(container, nullptr, context, callback, __func__);
}

ABX_API HRESULT XGameSaveEnumerateBlobInfoByName(XGameSaveContainerHandle container, const char* blobNamePrefix, void* context, XGameSaveBlobInfoCallback* callback) {
    return BlobInfo(container, blobNamePrefix, context, callback, __func__);
}

ABX_API HRESULT XGameSaveReadBlobData(XGameSaveContainerHandle container, const char** blobNames, std::uint32_t* countOfBlobs, std::size_t blobsSize, XGameSaveBlob* blobData) {
    if (container == nullptr || countOfBlobs == nullptr || (blobNames == nullptr && *countOfBlobs != 0) || blobData == nullptr) {
        return E_INVALIDARG;
    }
    std::vector<std::string> names;
    for (std::uint32_t i = 0; i < *countOfBlobs; ++i) {
        if (blobNames[i] == nullptr) {
            return E_INVALIDARG;
        }
        names.emplace_back(blobNames[i]);
    }
    std::vector<Blob> blobs;
    const HRESULT result = ReadBlobs(container, names, names.empty(), blobs, __func__);
    if (FAILED(result)) {
        return result;
    }
    if (LaidSize(blobs) > blobsSize) {
        return ProvidedBufferTooSmall;
    }
    Lay(blobs, blobData);
    *countOfBlobs = static_cast<std::uint32_t>(blobs.size());
    return S_OK;
}

ABX_API HRESULT XGameSaveReadBlobDataAsync(XGameSaveContainerHandle container, const char** blobNames, std::uint32_t countOfBlobs, XAsyncBlock* async) {
    if (container == nullptr || (blobNames == nullptr && countOfBlobs != 0)) {
        return E_INVALIDARG;
    }
    std::vector<std::string> names;
    for (std::uint32_t i = 0; i < countOfBlobs; ++i) {
        if (blobNames[i] == nullptr) {
            return E_INVALIDARG;
        }
        names.emplace_back(blobNames[i]);
    }
    return Start(async, reinterpret_cast<const void*>(&XGameSaveReadBlobDataAsync), __func__, [container, names](Operation& operation) {
        const HRESULT result = ReadBlobs(container, names, names.empty(), operation.blobs, "XGameSaveReadBlobDataAsync");
        operation.size = LaidSize(operation.blobs);
        operation.deliver = [&operation](void* buffer) { Lay(operation.blobs, buffer); };
        return result;
    });
}

ABX_API HRESULT XGameSaveReadBlobDataResult(XAsyncBlock* async, std::size_t blobsSize, XGameSaveBlob* blobData, std::uint32_t* countOfBlobs) {
    if (countOfBlobs == nullptr) {
        return E_INVALIDARG;
    }
    std::size_t used = 0;
    const HRESULT result = XAsyncGetResult(async, reinterpret_cast<const void*>(&XGameSaveReadBlobDataAsync), blobsSize, blobData, &used);
    if (result == static_cast<HRESULT>(0x8007007Au)) {
        return ProvidedBufferTooSmall;
    }
    *countOfBlobs = SUCCEEDED(result) && used != 0 ? static_cast<std::uint32_t>((reinterpret_cast<const std::uint8_t*>(blobData->info.name) - reinterpret_cast<const std::uint8_t*>(blobData)) / sizeof(XGameSaveBlob)) : 0;
    return result;
}

ABX_API HRESULT XGameSaveCreateUpdate(XGameSaveContainerHandle container, const char* containerDisplayName, XGameSaveUpdateHandle* updateContext) {
    if (container == nullptr || updateContext == nullptr) {
        return E_INVALIDARG;
    }
    XGameSaveUpdate* update = new (std::nothrow) XGameSaveUpdate();
    if (update == nullptr) {
        return E_OUTOFMEMORY;
    }
    update->root = container->root;
    update->name = container->name;
    update->keepDisplayName = containerDisplayName == nullptr;
    update->displayName = containerDisplayName == nullptr ? std::string() : containerDisplayName;
    *updateContext = update;
    return S_OK;
}

ABX_API void XGameSaveCloseUpdate(XGameSaveUpdateHandle context) {
    delete context;
}

ABX_API HRESULT XGameSaveSubmitBlobWrite(XGameSaveUpdateHandle updateContext, const char* blobName, const std::uint8_t* data, std::size_t byteCount) {
    return AddChange(updateContext, blobName, false, data, byteCount, __func__);
}

ABX_API HRESULT XGameSaveSubmitBlobDelete(XGameSaveUpdateHandle updateContext, const char* blobName) {
    return AddChange(updateContext, blobName, true, nullptr, 0, __func__);
}

ABX_API HRESULT XGameSaveSubmitUpdate(XGameSaveUpdateHandle updateContext) {
    if (updateContext == nullptr) {
        return E_INVALIDARG;
    }
    if (updateContext->submitted.exchange(true)) {
        return HandleExpired;
    }
    const HRESULT result = SubmitUpdate(updateContext, __func__);
    if (FAILED(result)) {
        updateContext->submitted = false;
    }
    return result;
}

ABX_API HRESULT XGameSaveSubmitUpdateAsync(XGameSaveUpdateHandle updateContext, XAsyncBlock* async) {
    if (updateContext == nullptr) {
        return E_INVALIDARG;
    }
    if (async == nullptr) {
        return E_INVALIDARG;
    }
    if (updateContext->submitted.exchange(true)) {
        return HandleExpired;
    }
    const HRESULT begun = Start(async, reinterpret_cast<const void*>(&XGameSaveSubmitUpdateAsync), __func__, [updateContext](Operation&) {
        const HRESULT result = SubmitUpdate(updateContext, "XGameSaveSubmitUpdateAsync");
        if (FAILED(result)) {
            updateContext->submitted = false;
        }
        return result;
    });
    if (FAILED(begun)) {
        updateContext->submitted = false;
    }
    return begun;
}

ABX_API HRESULT XGameSaveSubmitUpdateResult(XAsyncBlock* async) {
    return XAsyncGetResult(async, reinterpret_cast<const void*>(&XGameSaveSubmitUpdateAsync), 0, nullptr, nullptr);
}

ABX_API HRESULT XGameSaveFilesGetFolderWithUiAsync(XUserHandle, const char* configurationId, XAsyncBlock* async) {
    if (configurationId == nullptr) {
        return E_INVALIDARG;
    }
    return Start(async, reinterpret_cast<const void*>(&XGameSaveFilesGetFolderWithUiAsync), __func__, [](Operation& operation) {
        auto folder = std::make_shared<std::string>();
        const HRESULT result = SaveFilesFolder(*folder, "XGameSaveFilesGetFolderWithUiAsync");
        operation.size = folder->size() + 1;
        operation.deliver = [folder](void* buffer) { std::memcpy(buffer, folder->c_str(), folder->size() + 1); };
        return result;
    });
}

ABX_API HRESULT XGameSaveFilesGetFolderWithUiResult(XAsyncBlock* async, std::size_t folderSize, char* folderResult) {
    if (folderResult == nullptr) {
        return E_INVALIDARG;
    }
    const HRESULT result = XAsyncGetResult(async, reinterpret_cast<const void*>(&XGameSaveFilesGetFolderWithUiAsync), folderSize, folderResult, nullptr);
    return result == NotSufficientBuffer ? ProvidedBufferTooSmall : result;
}

ABX_API HRESULT XGameSaveFilesGetRemainingQuota(XUserHandle, const char* configurationId, std::int64_t* remainingQuota) {
    if (configurationId == nullptr || remainingQuota == nullptr) {
        return E_INVALIDARG;
    }
    std::wstring root;
    const HRESULT opened = OpenRoot(root, __func__);
    if (FAILED(opened)) {
        return opened;
    }
    return RemainingQuota(root, *remainingQuota, __func__);
}
