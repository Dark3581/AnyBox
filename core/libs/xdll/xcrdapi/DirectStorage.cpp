#include "xdll/xcrdapi/xcrdapi.hpp"

#include "Inflate.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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
HRESULT Unmapped(StorageObject* self, const void* first, const void* second, const void* third) {
    std::string arguments;
    for (const void* argument : {first, second, third}) {
        char line[200];
        std::snprintf(line, sizeof(line), " %p", argument);
        arguments += line;
        MEMORY_BASIC_INFORMATION region{};
        if (argument != nullptr && VirtualQuery(argument, &region, sizeof(region)) == sizeof(region) && region.State == MEM_COMMIT &&
            (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0 &&
            reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize - reinterpret_cast<std::uintptr_t>(argument) >= 64) {
            std::uint32_t words[16];
            std::memcpy(words, argument, sizeof(words));
            std::snprintf(line, sizeof(line), " [%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x]", words[0], words[1], words[2], words[3],
                          words[4], words[5], words[6], words[7], words[8], words[9], words[10], words[11], words[12], words[13], words[14], words[15]);
            arguments += line;
        }
    }
    const std::string message = std::string("slot ") + std::to_string(Slot) + " of the console DirectStorage " + self->name + " interface " + GuidText(*self->id) +
                                " is not mapped: no measured call site shows which method it is; arguments" + arguments;
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

constexpr GUID kStatusArrayInterface{0xcd66d752, 0x2786, 0x44f7, {0x90, 0x70, 0xe2, 0xa6, 0xfd, 0xad, 0x3f, 0x1b}};
constexpr GUID kQueueInterface{0x3c02cb0c, 0xc050, 0x4195, {0x99, 0xa1, 0xa1, 0xce, 0x94, 0xba, 0x33, 0xc5}};

struct StatusArray : StorageObject {
    std::vector<std::atomic<HRESULT>> results;
    std::vector<std::atomic<bool>> complete;
};

#pragma pack(push, 1)
struct ConsoleRequest {
    std::uint64_t options;
    std::uint64_t destination;
    std::uint64_t unknown10;
    std::uint32_t destinationSize;
    std::uint32_t unknown1c;
    std::uint64_t source;
    std::uint64_t sourceOffset;
    std::uint32_t sourceSize;
    std::uint32_t unknown34;
};
#pragma pack(pop)
static_assert(sizeof(ConsoleRequest) == 0x38);

constexpr std::uint64_t kRequestCompressed = 0x1;
constexpr std::uint64_t kRequestMemorySource = 0x400000;

struct StatusArray;

struct QueueEntry {
    ConsoleRequest request{};
    StatusArray* status = nullptr;
    std::uint32_t statusIndex = 0;
};

struct StorageQueue : StorageObject {
    std::mutex lock;
    std::vector<QueueEntry> pending;
    HRESULT failure = S_OK;
    std::uint64_t sourceType = 0;
    std::uint16_t capacity = 0;
    std::int8_t priority = 0;
    std::string label;
};

bool STDMETHODCALLTYPE StatusIsComplete(StorageObject* self, std::uint32_t index) {
    auto* status = static_cast<StatusArray*>(self);
    if (index >= status->complete.size()) {
        AbxUnsupported("DirectStorage IsComplete", "an index past the end of the status array");
    }
    return status->complete[index].load();
}

std::array<const void*, kSlots> StatusArrayTable() {
    std::array<const void*, kSlots> table = Table();
    table[3] = reinterpret_cast<const void*>(&StatusIsComplete);
    return table;
}

const std::array<const void*, kSlots> kStatusArrayTable = StatusArrayTable();
HRESULT STDMETHODCALLTYPE QueueEnqueueRequest(StorageQueue* self, const ConsoleRequest* request);
HRESULT STDMETHODCALLTYPE QueueEnqueueStatus(StorageQueue* self, StorageObject* array, std::uint32_t index);
HRESULT STDMETHODCALLTYPE QueueSubmit(StorageQueue* self);

HRESULT ExecuteRequest(const ConsoleRequest& request, std::string& error) {
    std::vector<std::uint8_t> source(request.sourceSize);
    if ((request.options & kRequestMemorySource) != 0) {
        std::memcpy(source.data(), reinterpret_cast<const void*>(request.source), source.size());
    } else {
        const auto* file = reinterpret_cast<const StorageFile*>(request.source);
        if (file == nullptr || file->handle == INVALID_HANDLE_VALUE) {
            error = "a request with no open file";
            return E_INVALIDARG;
        }
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(request.sourceOffset);
        at.OffsetHigh = static_cast<DWORD>(request.sourceOffset >> 32);
        DWORD read = 0;
        if (!ReadFile(file->handle, source.data(), static_cast<DWORD>(source.size()), &read, &at) || read != source.size()) {
            const DWORD code = GetLastError();
            error = "reading " + std::to_string(source.size()) + " bytes failed with " + std::to_string(code);
            return HRESULT_FROM_WIN32(code != 0 ? code : ERROR_HANDLE_EOF);
        }
    }
    auto* destination = reinterpret_cast<std::uint8_t*>(request.destination);
    if (destination == nullptr) {
        error = "a request with no destination";
        return E_INVALIDARG;
    }
    if ((request.options & kRequestCompressed) == 0) {
        if (source.size() > request.destinationSize) {
            error = "an uncompressed request larger than its destination";
            return E_INVALIDARG;
        }
        std::memcpy(destination, source.data(), source.size());
        return S_OK;
    }
    std::size_t written = 0;
    if (!abx::xcrdapi::InflateZlib(source.data(), source.size(), destination, request.destinationSize, written, error)) {
        return E_FAIL;
    }
    return S_OK;
}

std::array<const void*, kSlots> QueueTable() {
    std::array<const void*, kSlots> table = Table();
    table[3] = reinterpret_cast<const void*>(&QueueEnqueueRequest);
    table[4] = reinterpret_cast<const void*>(&QueueEnqueueStatus);
    table[6] = reinterpret_cast<const void*>(&QueueSubmit);
    return table;
}

const std::array<const void*, kSlots> kQueueTable = QueueTable();

void DestroyStatusArray(StorageObject* object) {
    delete static_cast<StatusArray*>(object);
}

void DestroyQueue(StorageObject* object) {
    delete static_cast<StorageQueue*>(object);
}

HRESULT STDMETHODCALLTYPE QueueEnqueueRequest(StorageQueue* self, const ConsoleRequest* request) {
    if (request == nullptr) {
        AbxUnsupported("DirectStorage EnqueueRequest", "a null request");
    }
    if ((request->options & ~(kRequestCompressed | kRequestMemorySource)) != 0 || request->unknown10 != 0 || request->unknown34 != 0) {
        char text[160];
        std::snprintf(text, sizeof(text), "request options 0x%llx or words at +0x10/+0x34 (0x%llx, 0x%x) that are not measured", static_cast<unsigned long long>(request->options),
                      static_cast<unsigned long long>(request->unknown10), request->unknown34);
        AbxUnsupported("DirectStorage EnqueueRequest", text);
    }
    static std::atomic<bool> reported{false};
    if ((request->options & kRequestCompressed) != 0 && (request->options & kRequestMemorySource) == 0 && !reported.exchange(true)) {
        const auto* file = reinterpret_cast<const StorageFile*>(request->source);
        unsigned char head[16]{};
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(request->sourceOffset);
        at.OffsetHigh = static_cast<DWORD>(request->sourceOffset >> 32);
        DWORD read = 0;
        ReadFile(file->handle, head, sizeof(head), &read, &at);
        AbxLog("xcrdapi", "first compressed request: %ls at 0x%llx, %u -> %u bytes, starts %02x %02x %02x %02x %02x %02x %02x %02x", file->path.c_str(),
               static_cast<unsigned long long>(request->sourceOffset), request->sourceSize, request->destinationSize, head[0], head[1], head[2], head[3], head[4], head[5],
               head[6], head[7]);
    }
    const std::lock_guard guard(self->lock);
    QueueEntry entry;
    entry.request = *request;
    self->pending.push_back(entry);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE QueueEnqueueStatus(StorageQueue* self, StorageObject* array, std::uint32_t index) {
    if (array == nullptr || array->id != &kStatusArrayInterface) {
        AbxUnsupported("DirectStorage EnqueueStatus", "a status array that AnyBox did not create");
    }
    auto* status = static_cast<StatusArray*>(array);
    if (index >= status->results.size()) {
        AbxUnsupported("DirectStorage EnqueueStatus", "an index past the end of the status array");
    }
    status->complete[index] = false;
    const std::lock_guard guard(self->lock);
    QueueEntry entry;
    entry.status = status;
    entry.statusIndex = index;
    self->pending.push_back(entry);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE QueueSubmit(StorageQueue* self) {
    std::vector<QueueEntry> entries;
    {
        const std::lock_guard guard(self->lock);
        entries.swap(self->pending);
    }
    HRESULT since = S_OK;
    for (const QueueEntry& entry : entries) {
        if (entry.status != nullptr) {
            entry.status->results[entry.statusIndex] = since;
            entry.status->complete[entry.statusIndex] = true;
            since = S_OK;
            continue;
        }
        std::string error;
        const HRESULT result = ExecuteRequest(entry.request, error);
        if (FAILED(result)) {
            AbxLog("xcrdapi", "DirectStorage request failed with 0x%08lx: %s", static_cast<unsigned long>(result), error.c_str());
            if (SUCCEEDED(since)) since = result;
            if (SUCCEEDED(self->failure)) self->failure = result;
        }
    }
    AbxTrace("DirectStorage queue \"%s\": submitted %zu entries", self->label.c_str(), entries.size());
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FactoryCreateStatusArray(StorageObject*, std::uint32_t capacity, const char* name, REFIID riid, void** array) {
    if (array == nullptr) return E_POINTER;
    *array = nullptr;
    if (capacity == 0 || std::memcmp(&riid, &kStatusArrayInterface, sizeof(GUID)) != 0) {
        AbxUnsupported("DirectStorage", ("CreateStatusArray with capacity 0 or interface " + GuidText(riid)).c_str());
    }
    StatusArray* created = new StatusArray();
    created->table = kStatusArrayTable.data();
    created->id = &kStatusArrayInterface;
    created->name = "status array";
    created->destroy = &DestroyStatusArray;
    created->results = std::vector<std::atomic<HRESULT>>(capacity);
    created->complete = std::vector<std::atomic<bool>>(capacity);
    AbxTrace("DirectStorage CreateStatusArray(%u, %s)", capacity, name != nullptr ? name : "");
    *array = created;
    return S_OK;
}

#pragma pack(push, 1)
struct ConsoleQueueDesc {
    std::uint64_t sourceType;
    std::uint16_t capacity;
    std::int8_t priority;
    std::uint8_t reserved[5];
    const char* name;
    std::uint64_t device;
};
#pragma pack(pop)
static_assert(sizeof(ConsoleQueueDesc) == 0x20);

HRESULT STDMETHODCALLTYPE FactoryCreateQueue(StorageObject*, const ConsoleQueueDesc* desc, REFIID riid, void** queue) {
    if (queue == nullptr) return E_POINTER;
    *queue = nullptr;
    if (desc == nullptr || std::memcmp(&riid, &kQueueInterface, sizeof(GUID)) != 0) {
        AbxUnsupported("DirectStorage", ("CreateQueue without a description or for interface " + GuidText(riid)).c_str());
    }
    StorageQueue* created = new StorageQueue();
    created->table = kQueueTable.data();
    created->id = &kQueueInterface;
    created->name = "queue";
    created->destroy = &DestroyQueue;
    created->sourceType = desc->sourceType;
    created->capacity = desc->capacity;
    created->priority = desc->priority;
    created->label = desc->name != nullptr ? desc->name : "";
    AbxLog("xcrdapi", "DirectStorage queue \"%s\": source type %llu, capacity %u, priority %d, word at +0x18 = %llu (recorded)", created->label.c_str(),
           static_cast<unsigned long long>(desc->sourceType), desc->capacity, desc->priority, static_cast<unsigned long long>(desc->device));
    *queue = created;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FactorySetStagingBufferSize(StorageObject*, std::uint32_t size) {
    AbxTrace("DirectStorage factory: staging buffer size %u bytes (reads go through the file system, no staging buffer is used)", size);
    return S_OK;
}

std::array<const void*, kSlots> FactoryTable() {
    std::array<const void*, kSlots> table = Table();
    table[4] = reinterpret_cast<const void*>(&FactoryOpenFile);
    table[5] = reinterpret_cast<const void*>(&FactoryCreateStatusArray);
    table[10] = reinterpret_cast<const void*>(&FactoryCreateQueue);
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
