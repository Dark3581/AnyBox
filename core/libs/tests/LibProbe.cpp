#include "AbxApi.hpp"

#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace {

struct XMemVirtualQueryInfo {
    MEMORY_BASIC_INFORMATION basic;
    std::uint8_t console[16];
};

using XMemAllocFn = void* (*)(std::size_t, std::uint64_t);
using XMemFreeFn = void (*)(void*, std::uint64_t);
using XMemVirtualQueryFn = BOOL (*)(HANDLE, const void*, XMemVirtualQueryInfo*);
using XMemGetWorkingSetStatisticsFn = BOOL (*)(std::uint32_t, void*);

std::wstring g_libs;

int Fail(const char* what) {
    std::printf("FAIL %s\n", what);
    return 1;
}

HMODULE Load(const wchar_t* name) {
    const std::wstring path = g_libs + L"\\" + name;
    return LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

template <typename Function>
Function Get(HMODULE module, const char* name) {
    return reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

bool Zero(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    return true;
}

int XMem() {
    const HMODULE module = Load(L"ab_xmem.dll");
    if (module == nullptr) return Fail("load ab_xmem.dll");
    const auto alloc = Get<XMemAllocFn>(module, "XMemAlloc");
    const auto free = Get<XMemFreeFn>(module, "XMemFree");
    const auto query = Get<XMemVirtualQueryFn>(module, "XMemVirtualQuery");
    const auto statistics = Get<XMemGetWorkingSetStatisticsFn>(module, "XMemGetWorkingSetStatistics");
    if (alloc == nullptr || free == nullptr || query == nullptr || statistics == nullptr) return Fail("xmem exports");

    for (const std::size_t size : {std::size_t{1}, std::size_t{100}, std::size_t{64 * 1024 - 1}, std::size_t{64 * 1024}, std::size_t{3 * 1024 * 1024 + 5}}) {
        auto* block = static_cast<std::uint8_t*>(alloc(size, 0x5e50000));
        if (block == nullptr) return Fail("XMemAlloc returned null");
        if (reinterpret_cast<std::uintptr_t>(block) % 64 != 0) return Fail("XMemAlloc alignment");
        if (!Zero(block, size)) return Fail("XMemAlloc zeroes the block");
        block[0] = 1;
        block[size - 1] = 2;

        XMemVirtualQueryInfo info;
        std::memset(&info, 0xCD, sizeof(info));
        if (!query(GetCurrentProcess(), block + size - 1, &info)) return Fail("XMemVirtualQuery");
        if (info.basic.State != MEM_COMMIT || (info.basic.Protect & PAGE_READWRITE) == 0) return Fail("XMemVirtualQuery state");
        const auto base = reinterpret_cast<std::uintptr_t>(info.basic.BaseAddress);
        const auto last = reinterpret_cast<std::uintptr_t>(block + size - 1);
        if (base > last || last >= base + info.basic.RegionSize) return Fail("XMemVirtualQuery region");
        if (!Zero(info.console, sizeof(info.console))) return Fail("XMemVirtualQuery console bytes");
        free(block, 0x5e50000);
    }

    XMemVirtualQueryInfo info;
    if (query(nullptr, &info, &info)) return Fail("XMemVirtualQuery with a null process");

    std::uint8_t buffer[32];
    std::memset(buffer, 0xAB, sizeof(buffer));
    if (statistics(0x20000001, buffer)) return Fail("XMemGetWorkingSetStatistics reports no statistics");
    for (const std::uint8_t byte : buffer) {
        if (byte != 0xAB) return Fail("XMemGetWorkingSetStatistics leaves the output alone");
    }
    std::puts("xmem ok");
    return 0;
}

template <typename Deserializer>
int CheckDeserialized(HMODULE module, const char* name, ID3DBlob* blob, D3D12_ROOT_SIGNATURE_FLAGS expected) {
    using CreateFn = HRESULT (*)(const void*, SIZE_T, REFIID, void**);
    const auto create = Get<CreateFn>(module, name);
    if (create == nullptr) return Fail(name);
    Deserializer* deserializer = nullptr;
    if (FAILED(create(blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(Deserializer), reinterpret_cast<void**>(&deserializer)))) return Fail(name);
    D3D12_ROOT_SIGNATURE_FLAGS flags{};
    if constexpr (std::is_same_v<Deserializer, ID3D12RootSignatureDeserializer>) {
        flags = deserializer->GetRootSignatureDesc()->Flags;
    } else {
        flags = deserializer->GetUnconvertedRootSignatureDesc()->Desc_1_1.Flags;
    }
    deserializer->Release();
    if (flags != expected) return Fail("deserialized flags");
    return 0;
}

int RootSignature() {
    const HMODULE module = Load(L"ab_d3d12_x.dll");
    if (module == nullptr) return Fail("load ab_d3d12_x.dll");
    const auto serialize = Get<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(module, "D3D12SerializeRootSignature");
    const auto serializeVersioned = Get<PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE>(module, "D3D12SerializeVersionedRootSignature");
    if (serialize == nullptr || serializeVersioned == nullptr) return Fail("serializer exports");

    const auto consoleFlags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(0x03000000u | D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    D3D12_ROOT_SIGNATURE_DESC legacy{};
    legacy.Flags = consoleFlags;
    ID3DBlob* blob = nullptr;
    ID3DBlob* error = nullptr;
    if (FAILED(serialize(&legacy, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error))) return Fail("D3D12SerializeRootSignature");
    if (legacy.Flags != consoleFlags) return Fail("D3D12SerializeRootSignature leaves the caller's description alone");
    if (CheckDeserialized<ID3D12RootSignatureDeserializer>(module, "D3D12CreateRootSignatureDeserializer", blob, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT) != 0) return 1;
    blob->Release();

    D3D12_ROOT_PARAMETER1 parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.Num32BitValues = 4;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned{};
    versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versioned.Desc_1_1.NumParameters = 1;
    versioned.Desc_1_1.pParameters = &parameter;
    versioned.Desc_1_1.Flags = consoleFlags;
    if (FAILED(serializeVersioned(&versioned, &blob, &error))) return Fail("D3D12SerializeVersionedRootSignature");
    if (CheckDeserialized<ID3D12VersionedRootSignatureDeserializer>(module, "D3D12CreateVersionedRootSignatureDeserializer", blob, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT) != 0) return 1;
    blob->Release();
    std::puts("root signature ok");
    return 0;
}

int FreeUnknown() {
    const HMODULE module = Load(L"ab_xmem.dll");
    if (module == nullptr) return Fail("load ab_xmem.dll");
    int local = 0;
    Get<XMemFreeFn>(module, "XMemFree")(&local, 0);
    return Fail("XMemFree returned");
}

int AllocZero() {
    const HMODULE module = Load(L"ab_xmem.dll");
    if (module == nullptr) return Fail("load ab_xmem.dll");
    Get<XMemAllocFn>(module, "XMemAlloc")(0, 0);
    return Fail("XMemAlloc returned");
}

int UnknownVersion() {
    const HMODULE module = Load(L"ab_d3d12_x.dll");
    if (module == nullptr) return Fail("load ab_d3d12_x.dll");
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = static_cast<D3D_ROOT_SIGNATURE_VERSION>(3);
    ID3DBlob* blob = nullptr;
    Get<PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE>(module, "D3D12SerializeVersionedRootSignature")(&desc, &blob, nullptr);
    return Fail("D3D12SerializeVersionedRootSignature returned");
}

#pragma pack(push, 1)
struct StorageRequest {
    std::uint64_t options;
    std::uint64_t destination;
    std::uint64_t pageOffset;
    std::uint32_t destinationSize;
    std::uint32_t unknown1c;
    std::uint64_t source;
    std::uint64_t sourceOffset;
    std::uint32_t sourceSize;
    std::uint32_t unknown34;
};

struct StorageQueueDesc {
    std::uint64_t sourceType;
    std::uint16_t capacity;
    std::int8_t priority;
    std::uint8_t reserved[5];
    const char* name;
    std::uint64_t device;
};
#pragma pack(pop)

template <typename Function>
Function Slot(void* object, std::size_t slot) {
    return reinterpret_cast<Function>((*static_cast<void***>(object))[slot]);
}

int StoragePages() {
    const HMODULE xmem = Load(L"ab_xmem.dll");
    const HMODULE storage = Load(L"ab_xcrdapi.dll");
    if (xmem == nullptr || storage == nullptr) return Fail("load ab_xmem.dll and ab_xcrdapi.dll");
    using AllocatePages = BOOL (*)(std::uint64_t, std::uintptr_t*, std::uintptr_t*);
    using PageView = void* (*)(std::uintptr_t);
    using GetFactory = HRESULT (*)(const GUID&, void**);
    using CreateStatusArray = HRESULT (*)(void*, std::uint32_t, const char*, const GUID&, void**);
    using CreateQueue = HRESULT (*)(void*, const StorageQueueDesc*, const GUID&, void**);
    using EnqueueRequest = HRESULT (*)(void*, const StorageRequest*);
    using EnqueueStatus = HRESULT (*)(void*, void*, std::uint32_t);
    using Submit = HRESULT (*)(void*);
    using IsComplete = bool (*)(void*, std::uint32_t);
    const auto allocate = Get<AllocatePages>(xmem, "XMemAllocatePhysicalPages");
    const auto view = Get<PageView>(xmem, "AbxXMemPhysicalPageView");
    const auto getFactory = Get<GetFactory>(storage, "DStorageGetFactory");
    if (allocate == nullptr || view == nullptr || getFactory == nullptr) return Fail("physical page and DirectStorage exports");

    constexpr GUID factoryId{0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
    constexpr GUID statusId{0xcd66d752, 0x2786, 0x44f7, {0x90, 0x70, 0xe2, 0xa6, 0xfd, 0xad, 0x3f, 0x1b}};
    constexpr GUID queueId{0x3c02cb0c, 0xc050, 0x4195, {0x99, 0xa1, 0xa1, 0xce, 0x94, 0xba, 0x33, 0xc5}};
    constexpr std::size_t kPage = 64 * 1024;
    constexpr std::uint64_t kMemorySource = 0x400000;
    constexpr std::uint64_t kDestinationPages = 0x200000;
    constexpr std::uint32_t kOffset = 0x20;

    std::uintptr_t count = 3;
    std::uintptr_t frames[3]{};
    if (!allocate(0, &count, frames) || count != 3) return Fail("XMemAllocatePhysicalPages");
    auto* const earlier = static_cast<std::uint8_t*>(view(frames[0]));
    auto* const later = static_cast<std::uint8_t*>(view(frames[1]));
    auto* const steady = static_cast<std::uint8_t*>(view(frames[2]));
    if (earlier == nullptr || later == nullptr || steady == nullptr) return Fail("AbxXMemPhysicalPageView");

    void* factory = nullptr;
    if (FAILED(getFactory(factoryId, &factory)) || factory == nullptr) return Fail("DStorageGetFactory");
    void* status = nullptr;
    if (FAILED(Slot<CreateStatusArray>(factory, 5)(factory, 4, "probe", statusId, &status)) || status == nullptr) return Fail("CreateStatusArray");
    StorageQueueDesc desc{};
    desc.capacity = 16;
    desc.name = "probe";
    void* queue = nullptr;
    if (FAILED(Slot<CreateQueue>(factory, 10)(factory, &desc, queueId, &queue)) || queue == nullptr) return Fail("CreateQueue");

    std::uint8_t data[4096];
    for (std::size_t index = 0; index < sizeof(data); ++index) data[index] = static_cast<std::uint8_t>(index * 7 + 3);
    std::uintptr_t moving[1] = {frames[0]};
    std::uintptr_t fixed[1] = {frames[2]};
    StorageRequest request{};
    request.options = kMemorySource | kDestinationPages;
    request.destination = reinterpret_cast<std::uint64_t>(moving);
    request.pageOffset = kOffset;
    request.destinationSize = sizeof(data);
    request.source = reinterpret_cast<std::uint64_t>(data);
    request.sourceSize = sizeof(data);
    if (FAILED(Slot<EnqueueRequest>(queue, 3)(queue, &request))) return Fail("EnqueueRequest with a page array");
    request.destination = reinterpret_cast<std::uint64_t>(fixed);
    if (FAILED(Slot<EnqueueRequest>(queue, 3)(queue, &request))) return Fail("EnqueueRequest with a second page array");
    moving[0] = frames[1];
    if (FAILED(Slot<EnqueueStatus>(queue, 4)(queue, status, 0))) return Fail("EnqueueStatus");
    if (FAILED(Slot<Submit>(queue, 6)(queue))) return Fail("Submit");
    for (int wait = 0; wait < 500 && !Slot<IsComplete>(status, 3)(status, 0); ++wait) Sleep(10);
    if (!Slot<IsComplete>(status, 3)(status, 0)) return Fail("the request did not complete");

    if (std::memcmp(later + kOffset, data, sizeof(data)) != 0) return Fail("the data is in the page the array named at Submit");
    if (!Zero(earlier, kPage)) return Fail("the page the array named at EnqueueRequest is untouched");
    if (!Zero(later, kOffset) || !Zero(later + kOffset + sizeof(data), kPage - kOffset - sizeof(data))) return Fail("nothing is written around the data");
    if (std::memcmp(steady + kOffset, data, sizeof(data)) != 0) return Fail("an array that did not change is written as before");
    std::puts("physical-page requests go to the pages their array names at Submit");
    return 0;
}

struct GuardProbe {
    std::uint32_t (*guard)(std::uint64_t, BOOL, std::uint64_t*, std::uint32_t) = nullptr;
    int returned = 0;
    std::uint64_t last = 0;
};

void OnGuardedPageWanted(void* context, std::uint64_t source) {
    auto* probe = static_cast<GuardProbe*>(context);
    ++probe->returned;
    probe->last = source;
    probe->guard(source & ~std::uint64_t{0xFFFF}, FALSE, nullptr, 0);
}

DWORD ProtectionAt(const void* address) {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(address, &region, sizeof(region)) != sizeof(region)) return 0;
    return region.Protect;
}

int GuardedPages() {
    const HMODULE xmem = Load(L"ab_xmem.dll");
    if (xmem == nullptr) return Fail("load ab_xmem.dll");
    using AllocatePages = BOOL (*)(std::uint64_t, std::uintptr_t*, std::uintptr_t*);
    using VirtualAllocFn = void* (*)(void*, std::size_t, std::uint32_t, std::uint64_t, std::uint32_t);
    using MapPages = void* (*)(void*, std::size_t, const std::uintptr_t*);
    using PageView = void* (*)(std::uintptr_t);
    using Track = void (*)(void (*)(void*, std::uint64_t), void*);
    using Guard = std::uint32_t (*)(std::uint64_t, BOOL, std::uint64_t*, std::uint32_t);
    using Copy = BOOL (*)(std::uintptr_t, std::size_t, const void*, std::size_t);
    const auto allocate = Get<AllocatePages>(xmem, "XMemAllocatePhysicalPages");
    const auto reserve = Get<VirtualAllocFn>(xmem, "XMemVirtualAlloc");
    const auto map = Get<MapPages>(xmem, "XMemMapPhysicalPages");
    const auto view = Get<PageView>(xmem, "AbxXMemPhysicalPageView");
    const auto track = Get<Track>(xmem, "AbxXMemTrackPhysicalViews");
    const auto guard = Get<Guard>(xmem, "AbxXMemGuardPhysicalPage");
    const auto copy = Get<Copy>(xmem, "AbxXMemCopyToPhysicalPage");
    if (allocate == nullptr || reserve == nullptr || map == nullptr || view == nullptr || track == nullptr || guard == nullptr || copy == nullptr) {
        return Fail("physical page, view tracking and guard exports");
    }
    constexpr std::size_t kPage = 64 * 1024;
    constexpr std::uint32_t kPhysicalWindow = 0x00400000u;
    constexpr std::uint32_t kRefused = 0xFFFFFFFFu;
    std::uintptr_t count = 2;
    std::uintptr_t frames[2]{};
    if (!allocate(0, &count, frames) || count != 2) return Fail("XMemAllocatePhysicalPages");
    auto* const window = static_cast<std::uint8_t*>(reserve(nullptr, 4 * kPage, MEM_RESERVE | kPhysicalWindow, 0, PAGE_READWRITE));
    if (window == nullptr) return Fail("a physical page window");
    if (map(window, 1, &frames[0]) != window) return Fail("XMemMapPhysicalPages before tracking");
    auto* const service = static_cast<std::uint8_t*>(view(frames[0]));
    if (service == nullptr) return Fail("AbxXMemPhysicalPageView");
    window[5] = 0x5a;
    if (service[5] != 0x5a) return Fail("the window and the service view show the same page");

    std::uint64_t views[8]{};
    const std::uint64_t source = reinterpret_cast<std::uint64_t>(service);
    if (guard(source, TRUE, views, 8) != kRefused) return Fail("a guard before tracking is refused");
    GuardProbe probe;
    probe.guard = guard;
    track(&OnGuardedPageWanted, &probe);
    if (guard(source, TRUE, views, 8) != 1 || views[0] != reinterpret_cast<std::uint64_t>(window)) return Fail("a view mapped before tracking is found and guarded");
    if (ProtectionAt(window) != PAGE_NOACCESS) return Fail("a guarded view has no access");
    if (service[5] != 0x5a) return Fail("the service view stays readable");

    if (map(window + kPage, 1, &frames[0]) != window + kPage) return Fail("mapping a guarded page at a second address");
    if (probe.returned != 1 || probe.last != source) return Fail("mapping a guarded page asks for it first");
    if (ProtectionAt(window) != PAGE_READWRITE || ProtectionAt(window + kPage) != PAGE_READWRITE) return Fail("both views are open after the page came back");
    if (window[kPage + 5] != 0x5a) return Fail("the second view shows the page");

    if (guard(source, TRUE, views, 1) != kRefused || ProtectionAt(window) != PAGE_READWRITE) return Fail("more views than the caller can take are refused and left open");
    if (guard(source, TRUE, views, 8) != 2) return Fail("both views are guarded");
    if (ProtectionAt(window) != PAGE_NOACCESS || ProtectionAt(window + kPage) != PAGE_NOACCESS) return Fail("both guarded views have no access");
    const std::uint8_t data[4] = {1, 2, 3, 4};
    if (!copy(frames[0], 16, data, sizeof(data))) return Fail("AbxXMemCopyToPhysicalPage");
    if (probe.returned != 2) return Fail("a copy into a guarded page asks for it first");
    if (std::memcmp(service + 16, data, sizeof(data)) != 0 || std::memcmp(window + 16, data, sizeof(data)) != 0) return Fail("the copy is in the page");
    if (copy(frames[0], kPage - 2, data, sizeof(data))) return Fail("a copy past the page's end is refused");

    if (guard(source, TRUE, views, 8) != 2 || guard(source, FALSE, nullptr, 0) != 2) return Fail("guard and release name both views");
    if (ProtectionAt(window) != PAGE_READWRITE || ProtectionAt(window + kPage) != PAGE_READWRITE) return Fail("released views are open");
    if (guard(source, 2, views, 8) != 2) return Fail("both views are guarded against writes");
    if (ProtectionAt(window) != PAGE_READONLY || ProtectionAt(window + kPage) != PAGE_READONLY) return Fail("views guarded against writes are read-only");
    if (window[16] != 1 || window[kPage + 17] != 2) return Fail("views guarded against writes stay readable");
    const int returnedBefore = probe.returned;
    if (!copy(frames[0], 20, data, sizeof(data)) || probe.returned != returnedBefore + 1) return Fail("a copy into a page guarded against writes asks for it first");
    if (ProtectionAt(window) != PAGE_READWRITE || window[20] != 1) return Fail("the page is open again and holds the copy");
    if (guard(source, 3, views, 8) != kRefused || ProtectionAt(window) != PAGE_READWRITE) return Fail("an unknown guard kind is refused");
    if (guard(source + 8, TRUE, views, 8) != kRefused || guard(0x1000, TRUE, views, 8) != kRefused) return Fail("an address that is no physical page is refused");
    if (map(window + kPage, 1, nullptr) != window + kPage) return Fail("unmapping the second view");
    if (guard(source, TRUE, views, 8) != 1) return Fail("an unmapped view is forgotten");
    track(nullptr, nullptr);
    if (ProtectionAt(window) != PAGE_READWRITE) return Fail("ending the tracking opens every guarded view");
    if (guard(source, TRUE, views, 8) != kRefused) return Fail("a guard after tracking ended is refused");
    std::puts("every view of a physical page is guarded and released together, and writers ask for a guarded page first");
    return 0;
}

struct RefusedProbe {
    int calls = 0;
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
    std::uint64_t caller = 0;
    bool write = false;
    bool open = true;
};

bool OnRefusedBuffer(void* context, std::uint64_t address, std::uint64_t bytes, bool write, std::uint64_t caller) {
    auto* probe = static_cast<RefusedProbe*>(context);
    ++probe->calls;
    probe->address = address;
    probe->bytes = bytes;
    probe->caller = caller;
    probe->write = write;
    if (!probe->open) return false;
    DWORD old = 0;
    return VirtualProtect(reinterpret_cast<void*>(address), bytes, PAGE_READWRITE, &old) != FALSE;
}

bool Finished(HANDLE file, BOOL result, OVERLAPPED& overlapped, DWORD& count) {
    if (result) return true;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    return GetOverlappedResult(file, &overlapped, &count, TRUE) != FALSE;
}

int RefusedBuffers() {
    const HMODULE runtime = Load(L"ab_runtime.dll");
    if (runtime == nullptr) return Fail("load ab_runtime.dll");
    using Handler = bool (*)(void*, std::uint64_t, std::uint64_t, bool, std::uint64_t);
    using Watch = bool (*)(Handler, void*);
    const auto watch = Get<Watch>(runtime, "AbxWatchRefusedBuffers");
    if (watch == nullptr) return Fail("AbxWatchRefusedBuffers export");
    wchar_t folder[MAX_PATH];
    wchar_t path[MAX_PATH];
    if (GetTempPathW(MAX_PATH, folder) == 0 || GetTempFileNameW(folder, L"abx", 0, path) == 0) return Fail("a temporary file name");
    constexpr DWORD kBytes = 2 * 65536;
    std::vector<std::uint8_t> pattern(kBytes);
    for (DWORD index = 0; index < kBytes; ++index) pattern[index] = static_cast<std::uint8_t>(index * 7 + (index >> 9));
    auto* const buffer = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (buffer == nullptr) return Fail("VirtualAlloc");
    const std::uint64_t place = reinterpret_cast<std::uint64_t>(buffer);
    const auto protect = [&](DWORD protection) {
        DWORD old = 0;
        return VirtualProtect(buffer + 65536, 65536, protection, &old) != FALSE;
    };
    const auto own = [](std::uint64_t caller) {
        HMODULE module = nullptr;
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(caller), &module) &&
               module == GetModuleHandleW(nullptr);
    };
    const auto size = [](HANDLE file) {
        LARGE_INTEGER bytes{};
        return GetFileSizeEx(file, &bytes) ? static_cast<long long>(bytes.QuadPart) : -1ll;
    };
    RefusedProbe probe;
    const DWORD modes[] = {FILE_ATTRIBUTE_NORMAL, FILE_FLAG_NO_BUFFERING, FILE_FLAG_OVERLAPPED, FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING};
    for (const DWORD mode : modes) {
        std::memcpy(buffer, pattern.data(), kBytes);
        HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, mode, nullptr);
        if (file == INVALID_HANDLE_VALUE) return Fail("create the file");
        OVERLAPPED overlapped{};
        DWORD count = 0;
        watch(nullptr, nullptr);
        if (!protect(PAGE_NOACCESS)) return Fail("VirtualProtect");
        SetLastError(0);
        if (WriteFile(file, buffer, kBytes, &count, &overlapped)) return Fail("WriteFile from a page without access succeeds");
        const DWORD writeError = GetLastError();
        const bool later = (mode & FILE_FLAG_OVERLAPPED) != 0 && writeError == ERROR_IO_PENDING;
        if (later) {
            if (GetOverlappedResult(file, &overlapped, &count, TRUE)) {
                std::printf("mode 0x%08lx: an overlapped WriteFile from a page without access is accepted and completes on this host, so this mode is not checked\n",
                            static_cast<unsigned long>(mode));
                if (!protect(PAGE_READWRITE)) return Fail("VirtualProtect");
                CloseHandle(file);
                continue;
            }
            std::printf("mode 0x%08lx: WriteFile from a page without access is accepted and fails later with %lu, the file holds %lld byte(s)\n", static_cast<unsigned long>(mode),
                        static_cast<unsigned long>(GetLastError()), size(file));
            probe = RefusedProbe{};
            watch(&OnRefusedBuffer, &probe);
            overlapped = OVERLAPPED{};
            if (WriteFile(file, buffer, kBytes, &count, &overlapped) || GetLastError() != ERROR_IO_PENDING || GetOverlappedResult(file, &overlapped, &count, TRUE) || probe.calls != 0) {
                return Fail("an overlapped WriteFile from a page without access still fails later, without the handler");
            }
            if (!protect(PAGE_READWRITE)) return Fail("VirtualProtect");
        } else {
            std::printf("mode 0x%08lx: WriteFile from a page without access fails with %lu, the file holds %lld byte(s)\n", static_cast<unsigned long>(mode),
                        static_cast<unsigned long>(writeError), size(file));
            if (writeError != ERROR_INVALID_USER_BUFFER && writeError != ERROR_NOACCESS) return Fail("WriteFile from a page without access fails with another error than ERROR_INVALID_USER_BUFFER or ERROR_NOACCESS");
            probe = RefusedProbe{};
            probe.open = false;
            if (!watch(&OnRefusedBuffer, &probe)) return Fail("the runtime did not replace ReadFile and WriteFile");
            overlapped = OVERLAPPED{};
            if (WriteFile(file, buffer, kBytes, &count, &overlapped) || GetLastError() != writeError) return Fail("a handler that hands nothing back leaves the WriteFile failure");
            if (probe.calls != 1 || probe.address != place || probe.bytes != kBytes || probe.write || !own(probe.caller)) return Fail("the WriteFile handler arguments");
            probe.open = true;
        }
        overlapped = OVERLAPPED{};
        count = 0;
        if (!Finished(file, WriteFile(file, buffer, kBytes, &count, &overlapped), overlapped, count) || count != kBytes) return Fail("WriteFile after the pages came back");
        if (probe.calls != (later ? 0 : 2) || size(file) != kBytes) return Fail("one hand-back was enough for WriteFile and the file has the buffer's size");

        std::memset(buffer, 0, kBytes);
        watch(nullptr, nullptr);
        if (!protect(PAGE_READONLY)) return Fail("VirtualProtect");
        overlapped = OVERLAPPED{};
        SetLastError(0);
        if (ReadFile(file, buffer, kBytes, &count, &overlapped)) return Fail("ReadFile into a read-only page succeeds");
        const DWORD readError = GetLastError();
        std::printf("mode 0x%08lx: ReadFile into a read-only page fails with %lu\n", static_cast<unsigned long>(mode), static_cast<unsigned long>(readError));
        if (readError != ERROR_NOACCESS) return Fail("ReadFile into a read-only page fails with another error than ERROR_NOACCESS");
        probe = RefusedProbe{};
        probe.open = false;
        watch(&OnRefusedBuffer, &probe);
        overlapped = OVERLAPPED{};
        if (ReadFile(file, buffer, kBytes, &count, &overlapped) || GetLastError() != ERROR_NOACCESS) return Fail("a handler that hands nothing back leaves the ReadFile failure");
        if (probe.calls != 1 || probe.address != place || probe.bytes != kBytes || !probe.write || !own(probe.caller)) return Fail("the ReadFile handler arguments");
        probe.open = true;
        overlapped = OVERLAPPED{};
        count = 0;
        if (!Finished(file, ReadFile(file, buffer, kBytes, &count, &overlapped), overlapped, count) || count != kBytes) return Fail("ReadFile after the pages came back");
        if (probe.calls != 2 || std::memcmp(buffer, pattern.data(), kBytes) != 0) return Fail("the repeated ReadFile filled the buffer with what the repeated WriteFile wrote");
        overlapped = OVERLAPPED{};
        overlapped.Offset = kBytes;
        SetLastError(0);
        const BOOL past = ReadFile(file, buffer, kBytes, &count, &overlapped);
        const DWORD pastError = GetLastError();
        if (past ? count != 0 : (pastError != ERROR_HANDLE_EOF && !(pastError == ERROR_IO_PENDING && !GetOverlappedResult(file, &overlapped, &count, TRUE)))) {
            return Fail("a read past the end ends as without the replacement");
        }
        if (probe.calls != 2) return Fail("a failure other than a refused buffer does not ask the handler");
        CloseHandle(file);
    }
    watch(nullptr, nullptr);
    if (!protect(PAGE_READONLY)) return Fail("VirtualProtect");
    DWORD count = 0;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return Fail("open the file");
    if (ReadFile(file, buffer, kBytes, &count, nullptr) || GetLastError() != ERROR_NOACCESS || probe.calls != 2) return Fail("after the handler is taken away the failure is the title's again");
    CloseHandle(file);

    file = CreateFileW(path, FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE || !protect(PAGE_READWRITE)) return Fail("open the file for appending");
    std::memcpy(buffer, pattern.data(), kBytes);
    if (!protect(PAGE_NOACCESS)) return Fail("VirtualProtect");
    probe = RefusedProbe{};
    watch(&OnRefusedBuffer, &probe);
    if (!WriteFile(file, buffer, kBytes, &count, nullptr) || count != kBytes || probe.calls != 1) return Fail("an appending WriteFile from a page without access is repeated");
    CloseHandle(file);
    watch(nullptr, nullptr);
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return Fail("open the file");
    std::vector<std::uint8_t> whole(3 * kBytes);
    if (size(file) != 2ll * kBytes || !ReadFile(file, whole.data(), 3 * kBytes, &count, nullptr) || count != 2 * kBytes ||
        std::memcmp(whole.data(), pattern.data(), kBytes) != 0 || std::memcmp(whole.data() + kBytes, pattern.data(), kBytes) != 0) {
        return Fail("the refused appending WriteFile left nothing behind: the file holds the buffer exactly twice");
    }
    CloseHandle(file);
    DeleteFileW(path);
    VirtualFree(buffer, 0, MEM_RELEASE);
    std::puts("ReadFile into and WriteFile from a guarded page ask the handler, are repeated when it handed pages back, and fail as before when it did not");
    return 0;
}

int StorageFiles() {
    const HMODULE xmem = Load(L"ab_xmem.dll");
    const HMODULE storage = Load(L"ab_xcrdapi.dll");
    if (xmem == nullptr || storage == nullptr) return Fail("load ab_xmem.dll and ab_xcrdapi.dll");
    using GetFactory = HRESULT (*)(const GUID&, void**);
    using OpenFile = HRESULT (*)(void*, const wchar_t*, const GUID&, void**);
    using CreateStatusArray = HRESULT (*)(void*, std::uint32_t, const char*, const GUID&, void**);
    using CreateQueue = HRESULT (*)(void*, const StorageQueueDesc*, const GUID&, void**);
    using EnqueueRequest = HRESULT (*)(void*, const StorageRequest*);
    using EnqueueStatus = HRESULT (*)(void*, void*, std::uint32_t);
    using Submit = HRESULT (*)(void*);
    using IsComplete = bool (*)(void*, std::uint32_t);
    const auto getFactory = Get<GetFactory>(storage, "DStorageGetFactory");
    if (getFactory == nullptr) return Fail("DirectStorage exports");
    constexpr GUID factoryId{0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
    constexpr GUID statusId{0xcd66d752, 0x2786, 0x44f7, {0x90, 0x70, 0xe2, 0xa6, 0xfd, 0xad, 0x3f, 0x1b}};
    constexpr GUID queueId{0x3c02cb0c, 0xc050, 0x4195, {0x99, 0xa1, 0xa1, 0xce, 0x94, 0xba, 0x33, 0xc5}};
    constexpr GUID fileId{0x43704f9f, 0x27c6, 0x4a9f, {0x88, 0x15, 0xd1, 0x65, 0x27, 0x89, 0x4d, 0xf4}};
    constexpr std::size_t kBlocks = 96;
    constexpr std::size_t kBlock = 48 * 1024;
    const auto pattern = [](std::size_t block, std::size_t index) { return static_cast<std::uint8_t>(block * 31 + index * 7 + (index >> 8)); };

    wchar_t folder[MAX_PATH];
    wchar_t path[MAX_PATH];
    if (GetTempPathW(MAX_PATH, folder) == 0 || GetTempFileNameW(folder, L"abx", 0, path) == 0) return Fail("a temporary file name");
    std::vector<std::uint8_t> content(kBlocks * kBlock);
    for (std::size_t block = 0; block < kBlocks; ++block) {
        for (std::size_t index = 0; index < kBlock; ++index) content[block * kBlock + index] = pattern(block, index);
    }
    const HANDLE out = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    if (out == INVALID_HANDLE_VALUE || !WriteFile(out, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) || written != content.size()) {
        return Fail("writing the temporary file");
    }
    CloseHandle(out);

    void* factory = nullptr;
    if (FAILED(getFactory(factoryId, &factory)) || factory == nullptr) return Fail("DStorageGetFactory");
    void* file = nullptr;
    if (FAILED(Slot<OpenFile>(factory, 4)(factory, path, fileId, &file)) || file == nullptr) return Fail("OpenFile");
    void* status = nullptr;
    if (FAILED(Slot<CreateStatusArray>(factory, 5)(factory, 4, "probe", statusId, &status)) || status == nullptr) return Fail("CreateStatusArray");
    StorageQueueDesc desc{};
    desc.capacity = 256;
    desc.name = "probe";
    void* queue = nullptr;
    if (FAILED(Slot<CreateQueue>(factory, 10)(factory, &desc, queueId, &queue)) || queue == nullptr) return Fail("CreateQueue");

    std::vector<std::uint8_t> apart(kBlocks * kBlock);
    std::vector<std::uint8_t> same(kBlock);
    for (int round = 0; round < 8; ++round) {
        std::fill(apart.begin(), apart.end(), std::uint8_t{0});
        for (std::size_t block = 0; block < kBlocks; ++block) {
            StorageRequest request{};
            request.destination = reinterpret_cast<std::uint64_t>(apart.data() + (kBlocks - 1 - block) * kBlock);
            request.destinationSize = kBlock;
            request.source = reinterpret_cast<std::uint64_t>(file);
            request.sourceOffset = block * kBlock;
            request.sourceSize = kBlock;
            if (FAILED(Slot<EnqueueRequest>(queue, 3)(queue, &request))) return Fail("EnqueueRequest from a file");
        }
        if (FAILED(Slot<EnqueueStatus>(queue, 4)(queue, status, 0))) return Fail("EnqueueStatus");
        for (std::size_t block : {std::size_t{5}, std::size_t{9}, std::size_t{70}}) {
            StorageRequest request{};
            request.destination = reinterpret_cast<std::uint64_t>(same.data());
            request.destinationSize = kBlock;
            request.source = reinterpret_cast<std::uint64_t>(file);
            request.sourceOffset = block * kBlock;
            request.sourceSize = kBlock;
            if (FAILED(Slot<EnqueueRequest>(queue, 3)(queue, &request))) return Fail("EnqueueRequest to one destination");
        }
        if (FAILED(Slot<EnqueueStatus>(queue, 4)(queue, status, 1))) return Fail("EnqueueStatus");
        if (FAILED(Slot<Submit>(queue, 6)(queue))) return Fail("Submit");
        for (int wait = 0; wait < 1000 && !Slot<IsComplete>(status, 3)(status, 1); ++wait) Sleep(10);
        if (!Slot<IsComplete>(status, 3)(status, 0) || !Slot<IsComplete>(status, 3)(status, 1)) return Fail("the file requests did not complete");
        for (std::size_t block = 0; block < kBlocks; ++block) {
            if (std::memcmp(apart.data() + (kBlocks - 1 - block) * kBlock, content.data() + block * kBlock, kBlock) != 0) return Fail("each file request's data is at its destination");
        }
        if (std::memcmp(same.data(), content.data() + 70 * kBlock, kBlock) != 0) return Fail("requests to one destination run in their order");
    }
    using Release = ULONG (*)(void*);
    Slot<Release>(file, 2)(file);
    DeleteFileW(path);
    std::puts("file requests fill their destinations, and requests to one destination keep their order");
    return 0;
}

int ExpectUnsupported(const char* scenario) {
    wchar_t self[MAX_PATH];
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) return Fail("GetModuleFileNameW");
    std::wstring command = L"\"" + std::wstring(self) + L"\" \"" + g_libs + L"\" ";
    for (const char* c = scenario; *c != '\0'; ++c) command += static_cast<wchar_t>(*c);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) return Fail("CreateProcessW");
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (code != static_cast<DWORD>(AbxExitUnsupported)) {
        std::printf("FAIL %s exited with 0x%lx, expected 0x%x\n", scenario, code, AbxExitUnsupported);
        return 1;
    }
    std::printf("%s stops as unsupported\n", scenario);
    return 0;
}

}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::puts("usage: lib_probe <libs folder> <scenario>");
        return 2;
    }
    for (const char* c = argv[1]; *c != '\0'; ++c) g_libs += static_cast<wchar_t>(*c == '/' ? '\\' : *c);
    const std::string scenario = argv[2];
    if (scenario == "xmem") return XMem();
    if (scenario == "rootsig") return RootSignature();
    if (scenario == "storage_pages") return StoragePages();
    if (scenario == "storage_files") return StorageFiles();
    if (scenario == "guarded_pages") return GuardedPages();
    if (scenario == "refused_buffers") return RefusedBuffers();
    if (scenario == "free_unknown") return FreeUnknown();
    if (scenario == "alloc_zero") return AllocZero();
    if (scenario == "rootsig_version") return UnknownVersion();
    if (scenario == "unsupported" && argc > 3) return ExpectUnsupported(argv[3]);
    return Fail("unknown scenario");
}
