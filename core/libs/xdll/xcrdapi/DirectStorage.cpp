#include "xdll/xcrdapi/xcrdapi.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace {

constexpr std::size_t kSlots = 64;

struct StorageObject {
    const void* const* table;
    const GUID* id;
    const char* name;
    std::atomic<ULONG> references{1};
    void (*destroy)(StorageObject*) = nullptr;
};

std::string GuidText(const GUID& guid) {
    char text[40];
    std::snprintf(text, sizeof(text), "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3, guid.Data4[0],
                  guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return text;
}

constexpr GUID kUnknownInterface{0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

HRESULT STDMETHODCALLTYPE ObjectQueryInterface(StorageObject* self, REFIID riid, void** object) {
    if (object == nullptr) return E_POINTER;
    if (std::memcmp(&riid, self->id, sizeof(GUID)) == 0 || std::memcmp(&riid, &kUnknownInterface, sizeof(GUID)) == 0) {
        *object = self;
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE ObjectAddRef(StorageObject* self) {
    return self->destroy != nullptr ? ++self->references : 1;
}

ULONG STDMETHODCALLTYPE ObjectRelease(StorageObject* self) {
    if (self->destroy == nullptr) return 1;
    const ULONG left = --self->references;
    if (left == 0) self->destroy(self);
    return left;
}

template <std::size_t Slot>
HRESULT Unmapped(StorageObject* self) {
    const std::string message = std::string("slot ") + std::to_string(Slot) + " of the console DirectStorage " + self->name + " interface " + GuidText(*self->id) +
                                " is not mapped: no measured call site shows which method it is";
    AbxUnsupported("DirectStorage", message.c_str());
}

template <std::size_t... Slots>
constexpr std::array<const void*, kSlots> UnmappedTable(std::index_sequence<Slots...>) {
    return {reinterpret_cast<const void*>(&Unmapped<Slots>)...};
}

std::array<const void*, kSlots> Table() {
    std::array<const void*, kSlots> table = UnmappedTable(std::make_index_sequence<kSlots>{});
    table[0] = reinterpret_cast<const void*>(&ObjectQueryInterface);
    table[1] = reinterpret_cast<const void*>(&ObjectAddRef);
    table[2] = reinterpret_cast<const void*>(&ObjectRelease);
    return table;
}

constexpr GUID kFactoryInterface{0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};

constexpr GUID kFileInterface{0x43704f9f, 0x27c6, 0x4a9f, {0x88, 0x15, 0xd1, 0x65, 0x27, 0x89, 0x4d, 0xf4}};

struct StorageFile : StorageObject {
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::wstring path;
};

std::array<const void*, kSlots> FileTable() {
    return Table();
}

const std::array<const void*, kSlots> kFileTable = FileTable();

void DestroyFile(StorageObject* object) {
    StorageFile* file = static_cast<StorageFile*>(object);
    if (file->handle != INVALID_HANDLE_VALUE) CloseHandle(file->handle);
    delete file;
}

HRESULT STDMETHODCALLTYPE FactoryOpenFile(StorageObject*, const wchar_t* path, REFIID riid, void** file) {
    if (file == nullptr) return E_POINTER;
    *file = nullptr;
    if (path == nullptr) return E_INVALIDARG;
    if (std::memcmp(&riid, &kFileInterface, sizeof(GUID)) != 0) {
        AbxUnsupported("DirectStorage", ("OpenFile for an interface other than the file interface: " + GuidText(riid)).c_str());
    }
    const std::size_t length = AbxMapConsolePath(path, false, nullptr, 0);
    std::wstring mapped(length, L'\0');
    AbxMapConsolePath(path, false, mapped.data(), mapped.size());
    mapped.resize(length - 1);
    const HANDLE handle = CreateFileW(mapped.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        AbxTrace("DirectStorage OpenFile(%ls) -> %ls failed with %lu", path, mapped.c_str(), error);
        return HRESULT_FROM_WIN32(error);
    }
    StorageFile* opened = new StorageFile();
    opened->table = kFileTable.data();
    opened->id = &kFileInterface;
    opened->name = "file";
    opened->destroy = &DestroyFile;
    opened->handle = handle;
    opened->path = mapped;
    AbxTrace("DirectStorage OpenFile(%ls) -> %ls", path, mapped.c_str());
    *file = opened;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FactorySetStagingBufferSize(StorageObject*, std::uint32_t size) {
    AbxTrace("DirectStorage factory: staging buffer size %u bytes (reads go through the file system, no staging buffer is used)", size);
    return S_OK;
}

std::array<const void*, kSlots> FactoryTable() {
    std::array<const void*, kSlots> table = Table();
    table[4] = reinterpret_cast<const void*>(&FactoryOpenFile);
    table[8] = reinterpret_cast<const void*>(&FactorySetStagingBufferSize);
    return table;
}

const std::array<const void*, kSlots> kFactoryTable = FactoryTable();
StorageObject g_factory{kFactoryTable.data(), &kFactoryInterface, "factory", {1}, nullptr};

}

ABX_API HRESULT DStorageGetFactory(REFIID riid, void** factory) {
    if (factory == nullptr) {
        return E_POINTER;
    }
    *factory = nullptr;
    if (std::memcmp(&riid, &kFactoryInterface, sizeof(GUID)) != 0) {
        const std::string message = "a DirectStorage factory interface other than " + GuidText(kFactoryInterface) + ": " + GuidText(riid);
        AbxUnsupported(__func__, message.c_str());
    }
    *factory = &g_factory;
    return S_OK;
}
