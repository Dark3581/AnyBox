#include "ConsoleObjects.hpp"
#include "FrameDump.hpp"
#include "LiveStats.hpp"
#include "TitleTextures.hpp"

#include "AbxApi.hpp"
#include "CommandAllocator.hpp"
#include "CommandList.hpp"
#include "PipelineObject.hpp"
#include "Presenter.hpp"
#include "Resource.hpp"
#include "RootSignature.hpp"
#include "CommandQueue.hpp"
#include "DescriptorHeap.hpp"
#include "Fence.hpp"

#include <atomic>
#include <cstdint>
#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <chrono>
#include <cstdarg>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

#if defined(_MSC_VER)
#include <intrin.h>
#define ABX_CALLER _ReturnAddress()
#else
#define ABX_CALLER __builtin_return_address(0)
#endif

namespace abx::d3d12x::console {
namespace {

TitleAddressSpace* g_addresses = nullptr;


std::atomic<UINT64> g_timestampFrequency{0};
std::atomic<const VulkanDevice*> g_vulkan{nullptr};

struct Label {
    std::uint64_t* address;
    std::uint64_t value;
};

std::atomic<long long> g_traceUntil{0};
std::atomic<int> g_traceLines{0};

bool Tracing() {
    const long long until = g_traceUntil.load();
    if (until == 0) return false;
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return now < until && g_traceLines.load() < 600;
}

void StartTracing() {
    long long expected = 0;
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (g_traceUntil.compare_exchange_strong(expected, now + 30000)) AbxLog("d3d12_x", "WTRACE: tracing memory waits, labels and submissions for 30 s");
}

void TraceLine(const char* format, ...) {
    if (!Tracing()) return;
    ++g_traceLines;
    char text[256];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    AbxLog("d3d12_x", "WTRACE %s", text);
}

std::mutex g_writesLock;
std::map<std::uint64_t, std::pair<std::uint64_t, const char*>> g_lastWrites;

void NoteWrite(std::uint64_t address, std::uint64_t value, const char* how) {
    const std::lock_guard guard(g_writesLock);
    if (g_lastWrites.size() > 65536) g_lastWrites.clear();
    g_lastWrites[address] = {value, how};
}

std::mutex g_labelLock;
std::map<const CommandList*, std::vector<Label>> g_labels;

struct MemoryWait {
    const volatile std::uint64_t* address;
    std::uint32_t comparison;
    std::uint64_t value;
    std::uint64_t mask = ~std::uint64_t{0};
    bool narrow = false;
};

std::map<const CommandList*, std::vector<MemoryWait>> g_waits;

bool ListWritesBefore(const CommandList* list, std::uint64_t address, std::uint64_t bytes) {
    const auto found = g_labels.find(list);
    if (found == g_labels.end()) return false;
    for (const Label& label : found->second) {
        const std::uint64_t at = reinterpret_cast<std::uintptr_t>(label.address);
        if (at < address + bytes && address < at + sizeof(std::uint64_t)) return true;
    }
    return false;
}

void ReportOrderedWait() {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "a memory wait on an address the same command list writes earlier is ordered by the list itself and is not checked on the CPU before submission (as xcl does)");
    }
}

void STDMETHODCALLTYPE ListWait64(Facade* self, std::uint64_t* address, std::uint32_t comparison, std::uint64_t value, std::uint64_t flags) {
    static_cast<void>(flags);
    if (address == nullptr || comparison < D3D12_COMPARISON_FUNC_NEVER || comparison > D3D12_COMPARISON_FUNC_ALWAYS) {
        AbxUnsupported("D3D12 command list slot 74 (Wait64BitValueX)", ("a null address or comparison " + std::to_string(comparison)).c_str());
    }
    if (comparison == D3D12_COMPARISON_FUNC_ALWAYS) return;
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "command list slot 74 is Wait64BitValueX(address, comparison, value, flags): the list and what follows on its queue are held until the value is in memory or promised by a submitted label (first: comparison %u, value %llu)",
               comparison, static_cast<unsigned long long>(value));
    }
    TraceLine("wait64 recorded list %p 0x%llx cmp %u value %llu", static_cast<const void*>(InnerOf<CommandList>(self)),
              static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address)), comparison, static_cast<unsigned long long>(value));
    const std::lock_guard guard(g_labelLock);
    if (ListWritesBefore(InnerOf<CommandList>(self), reinterpret_cast<std::uintptr_t>(address), 8)) {
        ReportOrderedWait();
        return;
    }
    g_waits[InnerOf<CommandList>(self)].push_back({address, comparison, value});
}

void ClearLabels(const CommandList* list) {
    const std::lock_guard guard(g_labelLock);
    g_labels.erase(list);
    g_waits.erase(list);
}

HRESULT STDMETHODCALLTYPE ListClose(Facade* self) {
    return InnerOf<CommandList>(self)->Close();
}

template <std::size_t Slot>
void STDMETHODCALLTYPE ListRecordedCommand(Facade*, std::uint32_t first, std::uint32_t second, std::uint32_t third, std::uint64_t fourth) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "console command list slot %zu (0x%x, 0x%x, 0x%x, 0x%llx): recorded and not applied, later calls are traced only", Slot, first, second, third,
               static_cast<unsigned long long>(fourth));
    } else {
        AbxTrace("console command list slot %zu (0x%x, 0x%x, 0x%x, 0x%llx): not applied", Slot, first, second, third, static_cast<unsigned long long>(fourth));
    }
}

struct GpuPageEntry {
    std::uint32_t operation;
    std::uint32_t value;
    std::uint32_t count;
};
static_assert(sizeof(GpuPageEntry) == 12);

struct GpuPageRecord {
    std::uint64_t address;
    std::uint32_t entries;
    std::uint32_t unused;
    const GpuPageEntry* list;
};
static_assert(sizeof(GpuPageRecord) == 24);

constexpr std::uint64_t kGpuPageBytes = 0x10000;

std::mutex g_poolLock;
std::map<const void*, std::pair<std::uint64_t, std::uint64_t>> g_pools;
constexpr std::uint32_t kMapPoolPage = 1;
constexpr std::uint32_t kMapPlaceholder = 2;

using PhysicalPageView = void* (*)(std::uintptr_t);

PhysicalPageView XmemExport(const char* name) {
    const HMODULE xmem = GetModuleHandleW(L"ab_xmem.dll");
    return xmem != nullptr ? reinterpret_cast<PhysicalPageView>(reinterpret_cast<void*>(GetProcAddress(xmem, name))) : nullptr;
}

PhysicalPageView PageView() {
    static const PhysicalPageView view = XmemExport("AbxXMemPhysicalPageView");
    return view;
}

using MirrorPage = BOOL (*)(std::uintptr_t, std::uintptr_t);

MirrorPage Mirror() {
    static const MirrorPage mirror = [] {
        const HMODULE xmem = GetModuleHandleW(L"ab_xmem.dll");
        return xmem != nullptr ? reinterpret_cast<MirrorPage>(reinterpret_cast<void*>(GetProcAddress(xmem, "AbxXMemMirrorPhysicalPage"))) : nullptr;
    }();
    return mirror;
}

std::uintptr_t ZeroFrame() {
    static const std::uintptr_t frame = [] {
        using Allocate = BOOL (*)(std::uint64_t, std::uintptr_t*, std::uintptr_t*);
        const HMODULE xmem = GetModuleHandleW(L"ab_xmem.dll");
        const auto allocate = xmem != nullptr ? reinterpret_cast<Allocate>(reinterpret_cast<void*>(GetProcAddress(xmem, "XMemAllocatePhysicalPages"))) : nullptr;
        std::uintptr_t count = 1;
        std::uintptr_t page = 0;
        if (allocate == nullptr || !allocate(0, &count, &page)) return std::uintptr_t{0};
        AbxLog("d3d12_x", "command queue slot 31 operation 2 maps one shared zero page (frame %llu) at the placeholder addresses, for the CPU and the GPU", static_cast<unsigned long long>(page));
        return page;
    }();
    return frame;
}

PhysicalPageView ViewAt() {
    static const PhysicalPageView view = XmemExport("AbxXMemPhysicalViewAt");
    return view;
}

[[noreturn]] void RefuseGpuPages(const std::string& text) {
    AbxUnsupported("D3D12 command queue slot 31 (GPU page mapping)", text.c_str());
}

HRESULT STDMETHODCALLTYPE QueueUpdateGpuPages(Facade*, UINT count, const GpuPageRecord* records, const void* pool, UINT poolPages, UINT sixth) {
    constexpr UINT kMaxRecords = 4096;
    constexpr std::uint32_t kMaxEntries = 65536;
    if (count > kMaxRecords || (count != 0 && records == nullptr)) RefuseGpuPages("more than 4096 records or a null record array");
    if (count == 0) return S_OK;
    if (g_addresses == nullptr) RefuseGpuPages("GPU page mapping before the device's address space exists");
    const PhysicalPageView view = PageView();
    const PhysicalPageView viewAt = ViewAt();
    if (view == nullptr || viewAt == nullptr) RefuseGpuPages("ab_xmem.dll has no AbxXMemPhysicalPageView / AbxXMemPhysicalViewAt export");
    std::pair<std::uint64_t, std::uint64_t> poolRange{0, 0};
    {
        const std::lock_guard guard(g_poolLock);
        const auto found = g_pools.find(pool);
        if (found != g_pools.end()) poolRange = found->second;
        for (const auto& [handle, range] : g_pools) {
            if (poolRange.second == 0 && range.first == reinterpret_cast<std::uint64_t>(pool)) poolRange = range;
        }
    }
    if (poolRange.second != 0 && static_cast<std::uint64_t>(poolPages) * kGpuPageBytes != poolRange.second) {
        char text[200];
        std::snprintf(text, sizeof(text), "the fifth argument (%u) is not the page count of the pool at 0x%llx (%llu pages)", poolPages, static_cast<unsigned long long>(poolRange.first),
                      static_cast<unsigned long long>(poolRange.second / kGpuPageBytes));
        RefuseGpuPages(text);
    }
    if (sixth != 0) RefuseGpuPages("a sixth argument other than 0 (" + std::to_string(sixth) + ")");
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "command queue slot 31 maps the title's physical pages at GPU-only addresses: records {address, entries, 12-byte entries {operation, value, pages}}; operation 1 maps the page mapped at page `value` of the page pool the third argument names (its start, registered through device slot 74; the fifth argument is its page count, %u), operation 2 maps a shared zero page - layout read from the title's caller, every page checked against AnyBox's physical page windows",
               poolPages);
    }
    for (UINT index = 0; index < count; ++index) {
        const GpuPageRecord& record = records[index];
        if (record.entries > kMaxEntries || (record.entries != 0 && record.list == nullptr)) RefuseGpuPages("a record with more than 65536 entries or no entry list");
        std::uint64_t at = record.address;
        for (std::uint32_t entry = 0; entry < record.entries; ++entry) {
            const GpuPageEntry& page = record.list[entry];
            if (page.count > kMaxEntries) RefuseGpuPages("an entry of more than 65536 pages");
            for (std::uint32_t step = 0; step < page.count; ++step, at += kGpuPageBytes) {
                void* source = nullptr;
                std::uintptr_t frame = 0;
                if (page.operation == kMapPoolPage) {
                    const std::uint64_t poolOffset = static_cast<std::uint64_t>(page.value + step) * kGpuPageBytes;
                    if (poolRange.second == 0 || poolOffset >= poolRange.second) {
                        char text[200];
                        std::snprintf(text, sizeof(text), "pool page %u of %s at GPU address 0x%llx", page.value + step, poolRange.second == 0 ? "a pool that device slot 74 did not register" : "past the end of its pool",
                                      static_cast<unsigned long long>(at));
                        RefuseGpuPages(text);
                    }
                    frame = static_cast<std::uintptr_t>(poolRange.first + poolOffset);
                    source = viewAt(frame);
                } else if (page.operation == kMapPlaceholder) {
                    frame = ZeroFrame();
                    if (frame == 0) RefuseGpuPages("no physical page could be allocated for the placeholder mapping");
                    using MapFrame = BOOL (*)(std::uintptr_t, std::uintptr_t);
                    static const MapFrame mapFrame = [] {
                        const HMODULE xmem = GetModuleHandleW(L"ab_xmem.dll");
                        return xmem != nullptr ? reinterpret_cast<MapFrame>(reinterpret_cast<void*>(GetProcAddress(xmem, "AbxXMemMapFrameAt"))) : nullptr;
                    }();
                    if (mapFrame != nullptr) mapFrame(static_cast<std::uintptr_t>(at), frame);
                    source = view(frame);
                } else {
                    char text[200];
                    std::snprintf(text, sizeof(text), "entry operation %u (value 0x%x, %u pages) at GPU address 0x%llx; only operations 1 and 2 are read from the title's caller", page.operation,
                                  page.value, page.count, static_cast<unsigned long long>(at));
                    RefuseGpuPages(text);
                }
                if (source == nullptr) {
                    char text[240];
                    std::snprintf(text, sizeof(text), "operation %u value 0x%x names 0x%llx for GPU address 0x%llx, which has no physical page mapped in AnyBox's windows (pool page) or is not a frame AnyBox issued (placeholder)",
                                  page.operation, page.value + step, static_cast<unsigned long long>(frame), static_cast<unsigned long long>(at));
                    RefuseGpuPages(text);
                }
                if (page.operation == kMapPoolPage) {
                    const MirrorPage mirror = Mirror();
                    const bool mirrored = mirror != nullptr && mirror(static_cast<std::uintptr_t>(at), frame) != FALSE;
                    static std::atomic<int> failures{0};
                    if (!mirrored && failures.fetch_add(1) < 6) {
                        MEMORY_BASIC_INFORMATION region{};
                        VirtualQuery(reinterpret_cast<const void*>(at), &region, sizeof(region));
                        AbxLog("d3d12_x", "DIAG slot 31 pool page %u could not be mapped for the CPU at 0x%llx (pool page at 0x%llx): state 0x%lx protection 0x%lx type 0x%lx allocation base %p",
                               page.value + step, static_cast<unsigned long long>(at), static_cast<unsigned long long>(frame), region.State, region.Protect, region.Type, region.AllocationBase);
                    }
                    static std::atomic<bool> reported{false};
                    if (!reported.exchange(true)) {
                        AbxLog("d3d12_x", "command queue slot 31 pool pages are also mapped for the CPU at their GPU address, as on the console (first 0x%llx: %s)", static_cast<unsigned long long>(at),
                               mirrored ? "mapped" : "not inside an xmem physical window, GPU only");
                    }
                }
                std::string error;
                if (!g_addresses->AliasPhysical(at, kGpuPageBytes, reinterpret_cast<std::uint64_t>(source), error)) {
                    RefuseGpuPages("aliasing a physical page at GPU address failed: " + error);
                }
            }
        }
    }
    return S_OK;
}

void STDMETHODCALLTYPE QueueExecuteCommandLists(Facade* self, UINT count, void* const* lists, std::uint64_t frameToken, std::uint32_t flags) {
    constexpr UINT kMaxLists = 64;
    if (count > kMaxLists || (count != 0 && lists == nullptr) || flags > 0xFF) {
        AbxUnsupported("D3D12 command queue slot 35", "more than 64 lists, a null list array or flags above 0xff");
    }
    static std::array<std::atomic<bool>, 256> seenFlags{};
    if (!seenFlags[flags].exchange(true)) {
        AbxLog("d3d12_x", "ExecuteCommandLists flags 0x%x seen (recorded, their meaning is not measured)", flags);
    }
    ID3D12CommandList* inner[kMaxLists]{};
    for (UINT index = 0; index < count; ++index) {
        inner[index] = static_cast<CommandList*>(FacadeInner(lists[index]));
        if (inner[index] == nullptr) AbxUnsupported("D3D12 command queue slot 35", "a command list that AnyBox did not hand out");
    }
    AbxTrace("ExecuteCommandLists(%u lists, frame token %llu)", count, static_cast<unsigned long long>(frameToken));
    CommandQueue* queue = InnerOf<CommandQueue>(self);
    if (Tracing()) {
        std::string ids;
        for (UINT index = 0; index < count; ++index) {
            char one[24];
            std::snprintf(one, sizeof(one), " %p", static_cast<void*>(inner[index]));
            ids += one;
        }
        TraceLine("execute queue %p thread %lu lists%s", static_cast<void*>(queue), GetCurrentThreadId(), ids.c_str());
    }
    if (TitleTextures* textures = TitleTextures::Active()) textures->Flush();
    std::vector<ID3D12CommandList*> segment;
    std::vector<CommandQueue::MemoryWait> segmentWaits;
    std::vector<CommandQueue::MemoryLabel> segmentLabels;
    const auto submit = [&] {
        if (segment.empty()) return;
        queue->ExecuteOrdered(static_cast<UINT>(segment.size()), segment.data(), std::move(segmentWaits), std::move(segmentLabels));
        segment.clear();
        segmentWaits.clear();
        segmentLabels.clear();
    };
    for (UINT index = 0; index < count; ++index) {
        const CommandList* list = static_cast<CommandList*>(inner[index]);
        std::vector<CommandQueue::MemoryWait> waits;
        std::vector<CommandQueue::MemoryLabel> labels;
        {
            const std::lock_guard guard(g_labelLock);
            if (const auto found = g_waits.find(list); found != g_waits.end()) {
                for (const MemoryWait& wait : found->second) {
                    const std::uint64_t at = reinterpret_cast<std::uintptr_t>(wait.address);
                    bool ordered = false;
                    for (const CommandQueue::MemoryLabel& label : segmentLabels) ordered = ordered || reinterpret_cast<std::uintptr_t>(label.address) == at;
                    if (ordered) {
                        ReportOrderedWait();
                        continue;
                    }
                    const std::uint8_t* mapped = g_addresses != nullptr ? g_addresses->HostAt(at) : nullptr;
                    CommandQueue::MemoryWait converted;
                    converted.address = mapped != nullptr ? mapped : reinterpret_cast<const std::uint8_t*>(at);
                    converted.titleAddress = at;
                    converted.narrow = wait.narrow;
                    converted.comparison = wait.comparison;
                    converted.value = wait.value;
                    converted.mask = wait.mask;
                    waits.push_back(converted);
                }
            }
            if (const auto found = g_labels.find(list); found != g_labels.end()) {
                for (const Label& label : found->second) labels.push_back({label.address, label.value});
            }
        }
        if (!waits.empty()) submit();
        segment.push_back(inner[index]);
        segmentWaits.insert(segmentWaits.end(), waits.begin(), waits.end());
        segmentLabels.insert(segmentLabels.end(), labels.begin(), labels.end());
    }
    submit();
}

HRESULT STDMETHODCALLTYPE QueueSignal(Facade* self, void* fence, UINT64 value) {
    auto* inner = static_cast<Fence*>(FacadeInner(fence));
    if (inner == nullptr) AbxUnsupported("D3D12 command queue slot 16", "a fence that AnyBox did not hand out");
    return InnerOf<CommandQueue>(self)->Signal(inner, value);
}

std::mutex g_presentLock;
std::unique_ptr<Presenter> g_presenter;

HRESULT STDMETHODCALLTYPE QueuePresent(Facade* self, UINT planeCount, const std::uint8_t* planes, const void* parameters) {
    constexpr std::size_t kPlaneResources = 0x10;
    static_cast<void>(parameters);
    if (planeCount == 0 || planes == nullptr) AbxUnsupported("D3D12 command queue slot 32", "a present without planes");
    void* const* resources = nullptr;
    std::memcpy(&resources, planes + kPlaneResources, sizeof(resources));
    Resource* image = resources != nullptr ? static_cast<Resource*>(FacadeInner(resources[0])) : nullptr;
    if (image == nullptr || image->IsBuffer()) AbxUnsupported("D3D12 command queue slot 32", "a present plane without a texture AnyBox handed out");
    if (!InnerOf<CommandQueue>(self)->Drain(std::chrono::milliseconds(300))) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) AbxLog("d3d12_x", "a present was shown while its queue still held work waiting for a fence or a memory value for over 300 ms");
    }
    if (TracingFrame()) {
        const D3D12_RESOURCE_DESC& traced = image->Texture().desc;
        AbxLog("d3d12_x", "TRACE present %p %llux%u f%d (%u planes)", static_cast<void*>(image->Texture().image), static_cast<unsigned long long>(traced.Width), traced.Height,
               static_cast<int>(traced.Format), planeCount);
        if (const VulkanDevice* device = g_vulkan.load()) DumpTracedFrame(*device, image, Stats().presents.load());
        DescriptorHeap::ReportForeignDescriptors();
        if (TitleTextures* textures = TitleTextures::Active()) textures->Report();
    }
    ++Stats().presents;
    StartLiveStats();
    if (TitleTextures* textures = TitleTextures::Active()) textures->Refresh();
    const std::lock_guard guard(g_presentLock);
    std::string error;
    static std::chrono::steady_clock::time_point failingSince{};
    static std::uint64_t dropped = 0;
    constexpr auto kGiveUp = std::chrono::seconds(120);
    const auto failed = [&](const char* what) {
        const auto now = std::chrono::steady_clock::now();
        if (failingSince == std::chrono::steady_clock::time_point{}) {
            failingSince = now;
            AbxLog("d3d12_x", "%s: %s; frames are not shown until the window's surface can be presented to again (display off, locked or changed), retried every frame for 120 s", what,
                   error.c_str());
        }
        ++dropped;
        g_presenter.reset();
        if (const VulkanDevice* probe = g_vulkan.load()) {
            std::string lost;
            if (!probe->Submit([](VkCommandBuffer) {}, lost)) AbxUnsupported("D3D12 command queue slot 32", (std::string(what) + " and the GPU no longer accepts work: " + lost).c_str());
        }
        if (now - failingSince > kGiveUp) AbxUnsupported("D3D12 command queue slot 32", (std::string(what) + " for 120 s: " + error).c_str());
    };
    if (!g_presenter) {
        const VulkanDevice* device = g_vulkan.load();
        const HWND window = FindProcessWindow();
        if (device == nullptr || window == nullptr) AbxUnsupported("D3D12 command queue slot 32", "presenting before the title's window or the Vulkan device exists");
        g_presenter = Presenter::Create(*device, window, error);
        if (!g_presenter) {
            if (failingSince == std::chrono::steady_clock::time_point{}) AbxUnsupported("D3D12 command queue slot 32", ("the swapchain could not be created: " + error).c_str());
            failed("the swapchain could not be created again");
            Sleep(16);
            return S_OK;
        }
        if (failingSince == std::chrono::steady_clock::time_point{}) AbxLog("d3d12_x", "presenting to the title's window (%u plane(s); only the first is shown)", planeCount);
    }
    const D3D12_RESOURCE_DESC& desc = image->Texture().desc;
    if (!g_presenter->Present(image->Texture().image, {static_cast<std::uint32_t>(desc.Width), desc.Height}, VK_IMAGE_LAYOUT_GENERAL, error)) {
        failed("presenting failed");
        Sleep(16);
        return S_OK;
    }
    if (failingSince != std::chrono::steady_clock::time_point{}) {
        AbxLog("d3d12_x", "presenting works again after %llu frames that were not shown", static_cast<unsigned long long>(dropped));
        failingSince = {};
        dropped = 0;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE QueueGetTimestampFrequency(Facade* self, UINT64* frequency) {
    return InnerOf<CommandQueue>(self)->GetTimestampFrequency(frequency);
}

HRESULT STDMETHODCALLTYPE AllocatorSetMemoryCallback(Facade*, void* callback, void* context) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "command allocator memory callback %p (context %p) recorded and not called: AnyBox records commands into Vulkan command buffers", callback, context);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ListReset(Facade* self, void* allocator, void* initial, std::uint64_t extra) {
    if (extra != 0) {
        AbxUnsupported("D3D12 command list slot 139", "a nonzero third argument, which is not measured");
    }
    auto* inner = static_cast<ID3D12CommandAllocator*>(static_cast<CommandAllocator*>(FacadeInner(allocator)));
    if (inner == nullptr) {
        AbxUnsupported("D3D12 command list slot 139", "a command allocator that AnyBox did not hand out");
    }
    ID3D12PipelineState* state = nullptr;
    if (initial != nullptr) {
        state = static_cast<PipelineStateObject*>(FacadeInner(initial));
        if (state == nullptr) AbxUnsupported("D3D12 command list slot 139", "a pipeline state that AnyBox did not hand out");
    }
    ClearLabels(InnerOf<CommandList>(self));
    return InnerOf<CommandList>(self)->Reset(inner, state);
}

void STDMETHODCALLTYPE ListSetDescriptorHeaps(Facade* self, UINT count, void* const* heaps) {
    constexpr UINT kMaxHeaps = 2;
    if (count > kMaxHeaps || (count != 0 && heaps == nullptr)) {
        AbxUnsupported("D3D12 command list slot 32", "more than two descriptor heaps or a null heap array");
    }
    ID3D12DescriptorHeap* inner[kMaxHeaps]{};
    for (UINT index = 0; index < count; ++index) {
        inner[index] = static_cast<DescriptorHeap*>(FacadeInner(heaps[index]));
        if (inner[index] == nullptr) AbxUnsupported("D3D12 command list slot 32", "a descriptor heap that AnyBox did not hand out");
    }
    InnerOf<CommandList>(self)->SetDescriptorHeaps(count, inner);
}

void STDMETHODCALLTYPE ListWriteValue(Facade*, std::uint32_t* address, std::uint32_t value, std::uint64_t third, std::uint32_t fourth) {
    if (address == nullptr || third != 0 || fourth != 0) {
        AbxUnsupported("D3D12 command list slot 71", "a null address or nonzero third or fourth arguments, which are not measured");
    }
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "command list slot 71 writes a 32-bit marker to title memory: AnyBox writes it when it is recorded, before the GPU would reach it");
    }
    *address = value;
    NoteWrite(reinterpret_cast<std::uintptr_t>(address), value, "a 32-bit marker (slot 71)");
}



void STDMETHODCALLTYPE ListWriteTimestamp(Facade* self, std::uint64_t* address, std::uint64_t value, std::uint64_t flush, std::uint32_t mode) {
    static_cast<void>(flush);
    if (address == nullptr) AbxUnsupported("D3D12 command list slot 72 (Write64BitValueBottomOfPipeX)", "a null address");
    if ((mode & 3u) == 0) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            AbxLog("d3d12_x", "command list slot 72 is Write64BitValueBottomOfPipeX(address, value, flush, flags): values are written after the list's GPU work completes, timestamps (flags & 3) when recorded");
        }
        TraceLine("label recorded list %p 0x%llx value %llu flags 0x%x", static_cast<const void*>(InnerOf<CommandList>(self)),
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address)), static_cast<unsigned long long>(value), mode);
        const std::lock_guard guard(g_labelLock);
        g_labels[InnerOf<CommandList>(self)].push_back({address, value});
        return;
    }
    const UINT64 frequency = g_timestampFrequency.load();
    LARGE_INTEGER now{};
    LARGE_INTEGER ticks{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&ticks);
    if (frequency == 0 || ticks.QuadPart == 0) {
        AbxUnsupported("D3D12 command list slot 72", "a timestamp before the device reported its timestamp frequency");
    }
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "command list slot 72 writes a 64-bit timestamp: AnyBox writes the CPU clock in GPU timestamp units when it is recorded");
    }
    const long double seconds = static_cast<long double>(now.QuadPart) / static_cast<long double>(ticks.QuadPart);
    *address = static_cast<std::uint64_t>(seconds * static_cast<long double>(frequency));
    NoteWrite(reinterpret_cast<std::uintptr_t>(address), *address, "a timestamp (slot 72)");
}

std::optional<D3D12_RESOURCE_STATES> StateOfLayout(D3D12_BARRIER_LAYOUT layout) {
    switch (layout) {
    case D3D12_BARRIER_LAYOUT_UNDEFINED:
    case D3D12_BARRIER_LAYOUT_COMMON:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON:
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COMMON: return D3D12_RESOURCE_STATE_COMMON;
    case D3D12_BARRIER_LAYOUT_GENERIC_READ:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ:
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_GENERIC_READ: return D3D12_RESOURCE_STATE_GENERIC_READ;
    case D3D12_BARRIER_LAYOUT_RENDER_TARGET: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS:
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_UNORDERED_ACCESS: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ: return D3D12_RESOURCE_STATE_DEPTH_READ;
    case D3D12_BARRIER_LAYOUT_SHADER_RESOURCE:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE:
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_SHADER_RESOURCE: return D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    case D3D12_BARRIER_LAYOUT_COPY_SOURCE:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_SOURCE:
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_SOURCE: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case D3D12_BARRIER_LAYOUT_COPY_DEST:
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_DEST:
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_DEST: return D3D12_RESOURCE_STATE_COPY_DEST;
    case D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE: return D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    case D3D12_BARRIER_LAYOUT_RESOLVE_DEST: return D3D12_RESOURCE_STATE_RESOLVE_DEST;
    default: return std::nullopt;
    }
}

void STDMETHODCALLTYPE ListBarrier(Facade* self, UINT groupCount, const D3D12_BARRIER_GROUP* groups) {
    const char* function = "D3D12 command list slot 146";
    if (groupCount != 0 && groups == nullptr) AbxUnsupported(function, "barrier groups without an array");
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    for (UINT group = 0; group < groupCount; ++group) {
        const D3D12_BARRIER_GROUP& entry = groups[group];
        if (entry.Type == D3D12_BARRIER_TYPE_GLOBAL || entry.Type == D3D12_BARRIER_TYPE_BUFFER) {
            D3D12_RESOURCE_BARRIER memory{};
            memory.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barriers.push_back(memory);
            continue;
        }
        if (entry.Type != D3D12_BARRIER_TYPE_TEXTURE) AbxUnsupported(function, ("barrier group type " + std::to_string(static_cast<int>(entry.Type))).c_str());
        for (UINT index = 0; index < entry.NumBarriers; ++index) {
            const D3D12_TEXTURE_BARRIER& texture = entry.pTextureBarriers[index];
            Resource* resource = static_cast<Resource*>(FacadeInner(texture.pResource));
            if (resource == nullptr) AbxUnsupported(function, "a texture that AnyBox did not hand out");
            std::optional<D3D12_RESOURCE_STATES> before = StateOfLayout(texture.LayoutBefore);
            std::optional<D3D12_RESOURCE_STATES> after = StateOfLayout(texture.LayoutAfter);
            constexpr UINT kConsoleDisplayLayout = 31;
            D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE;
            resource->GetHeapProperties(nullptr, &heapFlags);
            if ((static_cast<UINT>(heapFlags) & D3D12_HEAP_FLAG_ALLOW_DISPLAY) != 0) {
                static std::atomic<bool> reported{false};
                if (static_cast<UINT>(texture.LayoutBefore) == kConsoleDisplayLayout) before = D3D12_RESOURCE_STATE_COMMON;
                if (static_cast<UINT>(texture.LayoutAfter) == kConsoleDisplayLayout) after = D3D12_RESOURCE_STATE_COMMON;
                if ((static_cast<UINT>(texture.LayoutBefore) == kConsoleDisplayLayout || static_cast<UINT>(texture.LayoutAfter) == kConsoleDisplayLayout) &&
                    !reported.exchange(true)) {
                    AbxLog("d3d12_x", "barrier layout 31 (console-only) on a display surface is treated as the present state");
                }
            }
            if (!before || !after) {
                AbxUnsupported(function, ("barrier layouts " + std::to_string(static_cast<int>(texture.LayoutBefore)) + " -> " +
                                          std::to_string(static_cast<int>(texture.LayoutAfter)) + " that have no resource state")
                                             .c_str());
            }
            D3D12_RESOURCE_BARRIER transition{};
            transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            transition.Transition.pResource = resource;
            transition.Transition.StateBefore = *before;
            transition.Transition.StateAfter = *after;
            const D3D12_BARRIER_SUBRESOURCE_RANGE& range = texture.Subresources;
            if (range.NumMipLevels == 0) {
                transition.Transition.Subresource = range.IndexOrFirstMipLevel;
                barriers.push_back(transition);
                continue;
            }
            const D3D12_RESOURCE_DESC& desc = resource->Texture().desc;
            const UINT mips = resource->Texture().layout.info.mipLevels + resource->SkippedMips();
            const UINT layers = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
            const UINT planes = (std::max)(range.NumPlanes, 1u);
            for (UINT plane = range.FirstPlane; plane < range.FirstPlane + planes; ++plane) {
                for (UINT slice = range.FirstArraySlice; slice < range.FirstArraySlice + (std::max)(range.NumArraySlices, 1u); ++slice) {
                    for (UINT mip = range.IndexOrFirstMipLevel; mip < range.IndexOrFirstMipLevel + range.NumMipLevels; ++mip) {
                        transition.Transition.Subresource = mip + slice * mips + plane * mips * layers;
                        barriers.push_back(transition);
                    }
                }
            }
        }
    }
    if (!barriers.empty()) InnerOf<CommandList>(self)->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
}

ID3D12Resource* ResourceArgument(void* resource, const char* function);

void STDMETHODCALLTYPE ListResourceBarrierConsole(Facade* self, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    const char* function = "D3D12 command list slot 26 (ResourceBarrier)";
    if (count == 0) return;
    if (barriers == nullptr) AbxUnsupported(function, "barriers without a barrier array");
    std::vector<D3D12_RESOURCE_BARRIER> inner(barriers, barriers + count);
    for (D3D12_RESOURCE_BARRIER& barrier : inner) {
        switch (barrier.Type) {
        case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION: barrier.Transition.pResource = ResourceArgument(barrier.Transition.pResource, function); break;
        case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
            barrier.Aliasing.pResourceBefore = ResourceArgument(barrier.Aliasing.pResourceBefore, function);
            barrier.Aliasing.pResourceAfter = ResourceArgument(barrier.Aliasing.pResourceAfter, function);
            break;
        case D3D12_RESOURCE_BARRIER_TYPE_UAV: barrier.UAV.pResource = ResourceArgument(barrier.UAV.pResource, function); break;
        default: AbxUnsupported(function, ("barrier type " + std::to_string(static_cast<int>(barrier.Type))).c_str());
        }
    }
    InnerOf<CommandList>(self)->ResourceBarrier(count, inner.data());
}

void STDMETHODCALLTYPE ListSetPipelineState(Facade* self, void* pipeline) {
    auto* inner = dynamic_cast<ID3D12PipelineState*>(FacadeInner(pipeline));
    if (inner == nullptr) AbxUnsupported("D3D12 command list slot 25", "a pipeline argument that is not a pipeline state AnyBox handed out");
    InnerOf<CommandList>(self)->SetPipelineState(inner);
}

template <auto Method, typename... Arguments>
void STDMETHODCALLTYPE ListForward(Facade* self, Arguments... arguments) {
    (InnerOf<CommandList>(self)->*Method)(arguments...);
}

ID3D12Resource* ResourceArgument(void* resource, const char* function) {
    if (resource == nullptr) return nullptr;
    auto* inner = dynamic_cast<ID3D12Resource*>(FacadeInner(resource));
    if (inner == nullptr) AbxUnsupported(function, "a resource argument that is not a resource AnyBox handed out");
    return inner;
}

void STDMETHODCALLTYPE ListCopyMemoryToMemory(Facade* self, std::uint64_t destination, std::uint64_t source, std::uint64_t bytes, std::uint32_t flags) {
    static_cast<void>(flags);
    InnerOf<CommandList>(self)->CopyMemoryToMemory(destination, source, bytes, "D3D12 command list slot 62 (CopyMemoryToMemoryX)");
}

template <typename Value>
void FillPattern(Facade* self, std::uint64_t destination, std::uint64_t bytes, Value value, const char* function) {
    std::uint8_t pattern[16];
    for (std::size_t index = 0; index < sizeof(pattern); index += sizeof(Value)) std::memcpy(pattern + index, &value, sizeof(Value));
    InnerOf<CommandList>(self)->FillTitleMemory(destination, bytes, pattern, sizeof(Value), function);
}

void STDMETHODCALLTYPE ListFill32(Facade* self, std::uint64_t destination, std::uint64_t bytes, std::uint32_t value) {
    FillPattern(self, destination, bytes, value, "D3D12 command list slot 63 (FillMemoryWith32BitValueX)");
}

void STDMETHODCALLTYPE ListFill64(Facade* self, std::uint64_t destination, std::uint64_t bytes, std::uint64_t value) {
    FillPattern(self, destination, bytes, value, "D3D12 command list slot 64 (FillMemoryWith64BitValueX)");
}

void STDMETHODCALLTYPE ListFill128(Facade* self, std::uint64_t destination, std::uint64_t bytes, const std::uint8_t* value) {
    const char* function = "D3D12 command list slot 65 (FillMemoryWith128BitValueX)";
    if (value == nullptr) AbxUnsupported(function, "a null value");
    std::uint8_t pattern[16];
    std::memcpy(pattern, value, sizeof(pattern));
    InnerOf<CommandList>(self)->FillTitleMemory(destination, bytes, pattern, 16, function);
}

void STDMETHODCALLTYPE ListCopyBufferRegion(Facade* self, void* destination, UINT64 destinationOffset, void* source, UINT64 sourceOffset, UINT64 bytes) {
    const char* function = "D3D12 command list slot 15";
    InnerOf<CommandList>(self)->CopyBufferRegion(ResourceArgument(destination, function), destinationOffset, ResourceArgument(source, function), sourceOffset, bytes);
}

void STDMETHODCALLTYPE ListCopyTextureRegion(Facade* self, const D3D12_TEXTURE_COPY_LOCATION* destination, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* source,
                                             const D3D12_BOX* box) {
    const char* function = "D3D12 command list slot 16";
    if (destination == nullptr || source == nullptr) AbxUnsupported(function, "a null copy location");
    D3D12_TEXTURE_COPY_LOCATION to = *destination;
    D3D12_TEXTURE_COPY_LOCATION from = *source;
    to.pResource = ResourceArgument(to.pResource, function);
    from.pResource = ResourceArgument(from.pResource, function);
    const auto remap = [](D3D12_TEXTURE_COPY_LOCATION& location) {
        auto* resource = dynamic_cast<Resource*>(location.pResource);
        if (resource == nullptr || location.Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX || resource->SkippedMips() == 0) return true;
        const UINT skip = resource->SkippedMips();
        const UINT kept = resource->Texture().desc.MipLevels;
        const UINT original = kept + skip;
        const UINT mip = location.SubresourceIndex % original;
        const UINT slice = location.SubresourceIndex / original;
        if (mip < skip) return false;
        location.SubresourceIndex = (mip - skip) + slice * kept;
        return true;
    };
    if (!remap(to) || !remap(from)) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) AbxLog("d3d12_x", "copies into the top mips of title textures stored without them (texture memory reduction) are dropped");
        return;
    }
    InnerOf<CommandList>(self)->CopyTextureRegion(&to, x, y, z, &from, box);
}

void STDMETHODCALLTYPE ListCopyTextureRegionX(Facade* self, const D3D12_TEXTURE_COPY_LOCATION* destination, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* source,
                                              const D3D12_BOX* box, UINT flags) {
    static std::atomic<bool> reported{false};
    if (flags != 0 && !reported.exchange(true)) AbxLog("d3d12_x", "command list slot 85 (CopyTextureRegionX) copy flags 0x%x are recorded and not applied", flags);
    ListCopyTextureRegion(self, destination, x, y, z, source, box);
}

void STDMETHODCALLTYPE ListCopyResource(Facade* self, void* destination, void* source) {
    const char* function = "D3D12 command list slot 17";
    InnerOf<CommandList>(self)->CopyResource(ResourceArgument(destination, function), ResourceArgument(source, function));
}

void STDMETHODCALLTYPE ListResolveSubresource(Facade* self, void* destination, UINT destinationSubresource, void* source, UINT sourceSubresource, DXGI_FORMAT format) {
    const char* function = "D3D12 command list slot 19";
    InnerOf<CommandList>(self)->ResolveSubresource(ResourceArgument(destination, function), destinationSubresource, ResourceArgument(source, function), sourceSubresource, format);
}

void STDMETHODCALLTYPE ListSetComputeRootSignature(Facade* self, void* signature) {
    auto* inner = dynamic_cast<RootSignature*>(FacadeInner(signature));
    if (signature != nullptr && inner == nullptr) AbxUnsupported("D3D12 command list slot 33", "a root signature argument that is not a root signature AnyBox handed out");
    InnerOf<CommandList>(self)->SetComputeRootSignature(inner);
}

void STDMETHODCALLTYPE ListSetGraphicsRootSignature(Facade* self, void* signature) {
    auto* inner = dynamic_cast<RootSignature*>(FacadeInner(signature));
    if (signature != nullptr && inner == nullptr) AbxUnsupported("D3D12 command list slot 34", "a root signature argument that is not a root signature AnyBox handed out");
    InnerOf<CommandList>(self)->SetGraphicsRootSignature(inner);
}

void STDMETHODCALLTYPE ListSetPrimitiveTopology(Facade* self, D3D12_PRIMITIVE_TOPOLOGY topology) {
    InnerOf<CommandList>(self)->IASetPrimitiveTopology(topology);
}

void STDMETHODCALLTYPE ListExecuteIndirect(Facade* self, void* signature, UINT maxCount, void* arguments, UINT64 argumentOffset, void* count, UINT64 countOffset) {
    const char* function = "D3D12 command list slot 61 (ExecuteIndirect)";
    auto* inner = dynamic_cast<ID3D12CommandSignature*>(FacadeInner(signature));
    if (inner == nullptr) AbxUnsupported(function, "a command signature that AnyBox did not hand out");
    InnerOf<CommandList>(self)->ExecuteIndirect(inner, maxCount, ResourceArgument(arguments, function), argumentOffset, count != nullptr ? ResourceArgument(count, function) : nullptr, countOffset);
}

void STDMETHODCALLTYPE ListCopyResourceX(Facade* self, void* destination, void* source, UINT flags) {
    const char* function = "D3D12 command list slot 86 (CopyResourceX)";
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) AbxLog("d3d12_x", "command list slot 86 is CopyResourceX(destination, source, flags): in place it is a console compression operation (nothing to do); flags 0x%x recorded", flags);
    if (destination == source) return;
    InnerOf<CommandList>(self)->CopyResource(ResourceArgument(destination, function), ResourceArgument(source, function));
}

template <typename Value>
void WriteTitleValue(std::uint64_t address, Value value) {
    const std::uint8_t* mapped = g_addresses != nullptr ? g_addresses->HostAt(address) : nullptr;
    auto* target = mapped != nullptr ? reinterpret_cast<volatile Value*>(const_cast<std::uint8_t*>(mapped)) : reinterpret_cast<volatile Value*>(address);
    *target = value;
    NoteWrite(address, value, "a record-time write (slots 90-92)");
}

void ReportSlotOnce(std::atomic<bool>& reported, const char* text) {
    if (!reported.exchange(true)) AbxLog("d3d12_x", "%s", text);
}

void STDMETHODCALLTYPE ListDrawIndexedX(Facade* self, std::uint64_t address, DXGI_FORMAT format, UINT count, UINT flags) {
    static_cast<void>(flags);
    static std::atomic<bool> reported{false};
    ReportSlotOnce(reported, "command list slot 83 is DrawIndexedX(index address, index format, index count, flags): one instance of that many indices, the bound index buffer kept");
    InnerOf<CommandList>(self)->DrawIndexedAt(address, format, count, "D3D12 command list slot 83 (DrawIndexedX)");
}

void STDMETHODCALLTYPE ListSetPredicationBufferX(Facade*, const std::uint64_t* desc) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "command list slot 87 is SetPredicationBufferX({address, operation}*): predication is not applied, so predicated work always runs (first: %s)",
               desc == nullptr ? "cleared" : "set");
    }
}

void STDMETHODCALLTYPE ListKickoffX(Facade*) {
    static std::atomic<bool> reported{false};
    ReportSlotOnce(reported, "command list slot 95 is KickoffX(): the work recorded so far runs when the list is executed, not earlier");
}

ID3D12QueryHeap* QueryHeapArgument(void* heap, const char* function) {
    if (heap == nullptr) AbxUnsupported(function, "a null query heap");
    auto* inner = dynamic_cast<ID3D12QueryHeap*>(static_cast<IUnknown*>(FacadeInner(heap)));
    if (inner == nullptr) AbxUnsupported(function, "a query heap argument that AnyBox did not hand out");
    return inner;
}

void STDMETHODCALLTYPE ListBeginQuery(Facade* self, void* heap, D3D12_QUERY_TYPE type, UINT index) {
    InnerOf<CommandList>(self)->BeginQuery(QueryHeapArgument(heap, "D3D12 command list slot 28 (BeginQuery)"), type, index);
}

void STDMETHODCALLTYPE ListEndQuery(Facade* self, void* heap, D3D12_QUERY_TYPE type, UINT index) {
    InnerOf<CommandList>(self)->EndQuery(QueryHeapArgument(heap, "D3D12 command list slot 29 (EndQuery)"), type, index);
}

void STDMETHODCALLTYPE ListResolveQueryData(Facade* self, void* heap, D3D12_QUERY_TYPE type, UINT start, UINT count, void* destination, UINT64 offset) {
    const char* function = "D3D12 command list slot 30 (ResolveQueryData)";
    InnerOf<CommandList>(self)->ResolveQueryData(QueryHeapArgument(heap, function), type, start, count, ResourceArgument(destination, function), offset);
}

void STDMETHODCALLTYPE ListSetPredication(Facade* self, void* buffer, UINT64 offset, D3D12_PREDICATION_OP operation) {
    InnerOf<CommandList>(self)->SetPredication(ResourceArgument(buffer, "D3D12 command list slot 31 (SetPredication)"), offset, operation);
}

void* STDMETHODCALLTYPE ListAllocateTransient(Facade* self, UINT bytes, UINT alignment) {
    return InnerOf<CommandList>(self)->AllocateTransient(bytes, alignment, "D3D12 command list slot 148 (transient memory)");
}

void STDMETHODCALLTYPE ListDispatchX(Facade* self, UINT x, UINT y, UINT z, UINT flags) {
    static std::atomic<bool> reported{false};
    ReportSlotOnce(reported, "command list slot 80 is DispatchX(x, y, z, flags): a Dispatch, the flags recorded");
    static_cast<void>(flags);
    InnerOf<CommandList>(self)->Dispatch(x, y, z);
}

void STDMETHODCALLTYPE ListWrite32Top(Facade*, std::uint32_t* address, std::uint32_t value, std::uint32_t flags) {
    static_cast<void>(flags);
    if (address == nullptr) AbxUnsupported("D3D12 command list slot 90 (Write32BitValueTopOfPipeX)", "a null address");
    static std::atomic<bool> reported{false};
    ReportSlotOnce(reported, "command list slots 90/91/92 (32/64-bit top-of-pipe and bottom-of-shader writes) are written when recorded");
    WriteTitleValue<std::uint32_t>(reinterpret_cast<std::uintptr_t>(address), value);
}

void STDMETHODCALLTYPE ListWrite64Top(Facade*, std::uint64_t* address, std::uint64_t value, std::uint32_t flags) {
    if (address == nullptr) AbxUnsupported("D3D12 command list slot 91 (Write64BitValueTopOfPipeX)", "a null address");
    if ((flags & 0xCu) != 0) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        value = static_cast<std::uint64_t>(now.QuadPart);
    }
    WriteTitleValue<std::uint64_t>(reinterpret_cast<std::uintptr_t>(address), value);
}

void STDMETHODCALLTYPE ListWrite32BottomOfShader(Facade*, std::uint32_t* address, std::uint32_t value, std::uint32_t stage) {
    static_cast<void>(stage);
    if (address == nullptr) AbxUnsupported("D3D12 command list slot 92 (Write32BitValueBottomOfShaderX)", "a null address");
    WriteTitleValue<std::uint32_t>(reinterpret_cast<std::uintptr_t>(address), value);
}

void STDMETHODCALLTYPE ListWait32(Facade* self, std::uint32_t* address, std::uint32_t comparison, std::uint32_t value, std::uint32_t mask, std::uint32_t flags) {
    static_cast<void>(flags);
    if (address == nullptr || comparison < D3D12_COMPARISON_FUNC_NEVER || comparison > D3D12_COMPARISON_FUNC_ALWAYS) {
        AbxUnsupported("D3D12 command list slot 73 (Wait32BitValueX)", ("a null address or comparison " + std::to_string(comparison)).c_str());
    }
    if (comparison == D3D12_COMPARISON_FUNC_ALWAYS) return;
    static std::atomic<bool> reported{false};
    ReportSlotOnce(reported, "command list slot 73 is Wait32BitValueX(address, comparison, value, mask, flags): held like a 64-bit wait until the masked value is there or promised");
    const std::lock_guard guard(g_labelLock);
    if (ListWritesBefore(InnerOf<CommandList>(self), reinterpret_cast<std::uintptr_t>(address), 4)) {
        ReportOrderedWait();
        return;
    }
    g_waits[InnerOf<CommandList>(self)].push_back({reinterpret_cast<const volatile std::uint64_t*>(address), comparison, value, mask, true});
}

void STDMETHODCALLTYPE ListDepthBiasX(Facade*, FLOAT bias, FLOAT clamp, FLOAT slope) {
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "command list slot 96 is RSSetDepthBiasX(%f, %f, %f): recorded, not applied (pipelines keep their own depth bias)", bias, clamp, slope);
    }
}

void STDMETHODCALLTYPE ListToggle101(Facade*, UINT kind, UINT enable) {
    static std::atomic<std::uint32_t> seen{0};
    const std::uint32_t bit = 1u << ((kind & 15u) * 2 + (enable != 0 ? 1u : 0u));
    if ((seen.fetch_or(bit) & bit) == 0) {
        AbxLog("d3d12_x", "command list slot 101 (%u, %u): a begin/end toggle the title brackets clears with (game+0x50a6651/0x50a666a); recorded, not applied", kind, enable);
    }
}

void STDMETHODCALLTYPE ListDispatchMesh(Facade* self, UINT x, UINT y, UINT z) {
    const char* function = "D3D12 command list slot 129 (taken as DispatchMesh)";
    const PipelineStateObject* pipeline = InnerOf<CommandList>(self)->Pipeline();
    bool anyStage = false;
    for (const char* stage : {"vs", "hs", "ds", "gs", "ps", "cs", "as", "ms"}) anyStage = anyStage || (pipeline != nullptr && pipeline->HasStage(stage));
    const bool unbuiltMesh = pipeline != nullptr && pipeline->BindPoint() == VK_PIPELINE_BIND_POINT_GRAPHICS && pipeline->Pipeline() == VK_NULL_HANDLE && !anyStage;
    if (pipeline == nullptr || (!pipeline->HasStage("ms") && !unbuiltMesh)) {
        HMODULE caller = nullptr;
        void* const returnAddress = ABX_CALLER;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(returnAddress), &caller);
        std::string stages;
        for (const char* stage : {"vs", "hs", "ds", "gs", "ps", "cs", "as", "ms"}) {
            if (pipeline != nullptr && pipeline->HasStage(stage)) stages += std::string(" ") + stage;
        }
        char text[320];
        std::snprintf(text, sizeof(text), "arguments (%u, %u, %u) with %s bound (bind point %d, stages:%s), called from module+0x%llx: slot 129 is read as DispatchMesh only when a mesh pipeline is bound",
                      x, y, z, pipeline == nullptr ? "no pipeline" : "a pipeline without a mesh stage", pipeline != nullptr ? static_cast<int>(pipeline->BindPoint()) : -1,
                      stages.empty() ? " none" : stages.c_str(), static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(returnAddress) - reinterpret_cast<std::uintptr_t>(caller)));
        AbxUnsupported(function, text);
    }
    InnerOf<CommandList>(self)->DispatchMesh(x, y, z);
}

void STDMETHODCALLTYPE ListSetShadingRate(Facade*, D3D12_SHADING_RATE rate, const D3D12_SHADING_RATE_COMBINER* combiners) {
    const bool fullRate = rate == D3D12_SHADING_RATE_1X1 &&
                          (combiners == nullptr || (combiners[0] == D3D12_SHADING_RATE_COMBINER_PASSTHROUGH && combiners[1] == D3D12_SHADING_RATE_COMBINER_PASSTHROUGH));
    static std::atomic<bool> reported{false};
    if (!fullRate && !reported.exchange(true)) {
        AbxLog("d3d12_x", "command list slot 126 (shading rate) asked for rate %u with combiners %u/%u; AnyBox shades every pixel at full rate", static_cast<unsigned>(rate),
               combiners != nullptr ? static_cast<unsigned>(combiners[0]) : 0u, combiners != nullptr ? static_cast<unsigned>(combiners[1]) : 0u);
    }
}

void STDMETHODCALLTYPE ListSetCoverageMask(Facade*, std::uint64_t mask) {
    static std::atomic<bool> reported{false};
    if (mask != ~0ull && !reported.exchange(true)) {
        AbxLog("d3d12_x", "command list slot 142 set the dithered coverage mask 0x%llx (four 16-bit groups); AnyBox draws with full coverage, so faded geometry shows solid",
               static_cast<unsigned long long>(mask));
    }
}

void STDMETHODCALLTYPE ListSetDepthBounds(Facade*, FLOAT minimum, FLOAT maximum) {
    AbxTrace("console command list slot 70: depth bounds %g..%g; no pipeline AnyBox builds enables the depth bounds test", static_cast<double>(minimum), static_cast<double>(maximum));
}

void STDMETHODCALLTYPE ListClearRenderTargetView(Facade* self, D3D12_CPU_DESCRIPTOR_HANDLE target, const FLOAT color[4], UINT rectCount, const D3D12_RECT* rects) {
    InnerOf<CommandList>(self)->ClearRenderTargetView(target, color, rectCount, rects);
}

struct FenceMirror {
    Fence* fence;
    std::atomic<UINT64> value;
};

std::mutex g_mirrorLock;
std::vector<std::unique_ptr<FenceMirror>> g_mirrors;
std::once_flag g_mirrorThread;

void MirrorFences() {
    for (;;) {
        {
            const std::lock_guard guard(g_mirrorLock);
            for (const auto& mirror : g_mirrors) mirror->value = mirror->fence->GetCompletedValue();
        }
        Sleep(1);
    }
}

HRESULT STDMETHODCALLTYPE AllocatorReset(Facade* self) {
    return InnerOf<CommandAllocator>(self)->Reset();
}

UINT64 STDMETHODCALLTYPE FenceGetCompletedValue(Facade* self) {
    return InnerOf<Fence>(self)->GetCompletedValue();
}

const std::atomic<UINT64>* FenceCompletedAddress(Facade* self) {
    Fence* fence = InnerOf<Fence>(self);
    std::call_once(g_mirrorThread, [] {
        std::thread(&MirrorFences).detach();
        AbxLog("d3d12_x", "fence slot 12 hands out a memory copy of each fence's completed value, refreshed about every millisecond");
    });
    const std::lock_guard guard(g_mirrorLock);
    for (const auto& mirror : g_mirrors) {
        if (mirror->fence == fence) return &mirror->value;
    }
    fence->AddRef();
    auto mirror = std::make_unique<FenceMirror>();
    mirror->fence = fence;
    mirror->value = fence->GetCompletedValue();
    g_mirrors.push_back(std::move(mirror));
    return &g_mirrors.back()->value;
}

HRESULT STDMETHODCALLTYPE ResourceMap(Facade* self, UINT subresource, const D3D12_RANGE* range, void** data) {
    Resource* resource = InnerOf<Resource>(self);
    if (!resource->IsBuffer()) {
        const D3D12_GPU_VIRTUAL_ADDRESS address = resource->GetGPUVirtualAddress();
        if (address == 0 || subresource != 0) {
            AbxUnsupported("D3D12 resource slot 8", "mapping a texture that has no title memory, or a subresource other than 0");
        }
        if (data != nullptr) *data = reinterpret_cast<void*>(address);
        return S_OK;
    }
    return resource->Map(subresource, range, data);
}

void STDMETHODCALLTYPE ResourceUnmap(Facade* self, UINT subresource, const D3D12_RANGE* range) {
    Resource* resource = InnerOf<Resource>(self);
    if (!resource->IsBuffer()) return;
    resource->Unmap(subresource, range);
}

D3D12_RESOURCE_DESC* ResourceGetDesc(Facade* self, D3D12_RESOURCE_DESC* desc) {
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    InnerOf<Resource>(self)->GetDesc(desc);
#else
    *desc = InnerOf<Resource>(self)->GetDesc();
#endif
    return desc;
}

D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE ResourceGetGpuVirtualAddress(Facade* self) {
    return InnerOf<Resource>(self)->GetGPUVirtualAddress();
}

SIZE_T HeapCpuStart(Facade* self) {
    return InnerOf<DescriptorHeap>(self)->CpuStart().ptr;
}

UINT64 HeapGpuStart(Facade* self) {
    return InnerOf<DescriptorHeap>(self)->GpuStart().ptr;
}

template <typename Function>
void Map(FacadeType& type, std::size_t slot, Function function) {
    type.table[slot] = reinterpret_cast<const void*>(function);
}

const FacadeType& TypeOf(Kind kind) {
    static const FacadeType queue = [] {
        FacadeType type = MakeFacadeType("command queue", {__uuidof(ID3D12CommandQueue)});
        Map(type, 16, &QueueSignal);
        Map(type, 18, &QueueGetTimestampFrequency);
        Map(type, 31, &QueueUpdateGpuPages);
        Map(type, 32, &QueuePresent);
        Map(type, 35, &QueueExecuteCommandLists);
        return type;
    }();
    static const FacadeType allocator = [] {
        FacadeType type = MakeFacadeType("command allocator", {__uuidof(ID3D12CommandAllocator)});
        Map(type, 8, &AllocatorReset);
        Map(type, 9, &AllocatorSetMemoryCallback);
        return type;
    }();
    static const FacadeType list = [] {
        FacadeType type = MakeFacadeType("command list", {kCommandListInterface, __uuidof(ID3D12GraphicsCommandList)});
        Map(type, 9, &ListClose);
        Map(type, 12, &ListForward<&CommandList::DrawInstanced, UINT, UINT, UINT, UINT>);
        Map(type, 13, &ListForward<&CommandList::DrawIndexedInstanced, UINT, UINT, UINT, INT, UINT>);
        Map(type, 14, &ListForward<&CommandList::Dispatch, UINT, UINT, UINT>);
        Map(type, 15, &ListCopyBufferRegion);
        Map(type, 16, &ListCopyTextureRegion);
        Map(type, 17, &ListCopyResource);
        Map(type, 19, &ListResolveSubresource);
        Map(type, 20, &ListSetPrimitiveTopology);
        Map(type, 21, &ListForward<&CommandList::RSSetViewports, UINT, const D3D12_VIEWPORT*>);
        Map(type, 22, &ListForward<&CommandList::RSSetScissorRects, UINT, const D3D12_RECT*>);
        Map(type, 23, &ListForward<&CommandList::OMSetBlendFactor, const FLOAT*>);
        Map(type, 24, &ListForward<&CommandList::OMSetStencilRef, UINT>);
        Map(type, 25, &ListSetPipelineState);
        Map(type, 26, &ListResourceBarrierConsole);
        Map(type, 32, &ListSetDescriptorHeaps);
        Map(type, 33, &ListSetComputeRootSignature);
        Map(type, 34, &ListSetGraphicsRootSignature);
        Map(type, 35, &ListForward<&CommandList::SetComputeRootDescriptorTable, UINT, D3D12_GPU_DESCRIPTOR_HANDLE>);
        Map(type, 36, &ListForward<&CommandList::SetGraphicsRootDescriptorTable, UINT, D3D12_GPU_DESCRIPTOR_HANDLE>);
        Map(type, 37, &ListForward<&CommandList::SetComputeRoot32BitConstant, UINT, UINT, UINT>);
        Map(type, 38, &ListForward<&CommandList::SetGraphicsRoot32BitConstant, UINT, UINT, UINT>);
        Map(type, 39, &ListForward<&CommandList::SetComputeRoot32BitConstants, UINT, UINT, const void*, UINT>);
        Map(type, 40, &ListForward<&CommandList::SetGraphicsRoot32BitConstants, UINT, UINT, const void*, UINT>);
        Map(type, 41, &ListForward<&CommandList::SetComputeRootConstantBufferView, UINT, D3D12_GPU_VIRTUAL_ADDRESS>);
        Map(type, 42, &ListForward<&CommandList::SetGraphicsRootConstantBufferView, UINT, D3D12_GPU_VIRTUAL_ADDRESS>);
        Map(type, 43, &ListForward<&CommandList::SetComputeRootShaderResourceView, UINT, D3D12_GPU_VIRTUAL_ADDRESS>);
        Map(type, 44, &ListForward<&CommandList::SetGraphicsRootShaderResourceView, UINT, D3D12_GPU_VIRTUAL_ADDRESS>);
        Map(type, 45, &ListForward<&CommandList::SetComputeRootUnorderedAccessView, UINT, D3D12_GPU_VIRTUAL_ADDRESS>);
        Map(type, 46, &ListForward<&CommandList::SetGraphicsRootUnorderedAccessView, UINT, D3D12_GPU_VIRTUAL_ADDRESS>);
        Map(type, 47, &ListForward<&CommandList::IASetIndexBuffer, const D3D12_INDEX_BUFFER_VIEW*>);
        Map(type, 48, &ListForward<&CommandList::IASetVertexBuffers, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*>);
        Map(type, 49, &ListForward<&CommandList::SOSetTargets, UINT, UINT, const D3D12_STREAM_OUTPUT_BUFFER_VIEW*>);
        Map(type, 50, &ListForward<&CommandList::OMSetRenderTargets, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*>);
        Map(type, 51, &ListForward<&CommandList::ClearDepthStencilView, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*>);
        Map(type, 52, &ListClearRenderTargetView);
        Map(type, 62, &ListCopyMemoryToMemory);
        Map(type, 70, &ListSetDepthBounds);
        Map(type, 71, &ListWriteValue);
        Map(type, 72, &ListWriteTimestamp);
        Map(type, 74, &ListWait64);
        Map(type, 139, &ListReset);
        Map(type, 142, &ListSetCoverageMask);
        Map(type, 148, &ListAllocateTransient);
        Map(type, 146, &ListBarrier);
        Map(type, 126, &ListSetShadingRate);
        Map(type, 129, &ListDispatchMesh);
        Map(type, 85, &ListCopyTextureRegionX);
        Map(type, 101, &ListToggle101);
        Map(type, 61, &ListExecuteIndirect);
        Map(type, 86, &ListCopyResourceX);
        Map(type, 80, &ListDispatchX);
        Map(type, 28, &ListBeginQuery);
        Map(type, 29, &ListEndQuery);
        Map(type, 30, &ListResolveQueryData);
        Map(type, 31, &ListSetPredication);
        Map(type, 83, &ListDrawIndexedX);
        Map(type, 87, &ListSetPredicationBufferX);
        Map(type, 95, &ListKickoffX);
        Map(type, 63, &ListFill32);
        Map(type, 64, &ListFill64);
        Map(type, 65, &ListFill128);
        Map(type, 90, &ListWrite32Top);
        Map(type, 91, &ListWrite64Top);
        Map(type, 92, &ListWrite32BottomOfShader);
        Map(type, 73, &ListWait32);
        Map(type, 96, &ListDepthBiasX);
        Map(type, 75, &ListRecordedCommand<75>);
        Map(type, 131, &ListRecordedCommand<131>);
        Map(type, 132, &ListRecordedCommand<132>);
        return type;
    }();
    static const FacadeType fence = [] {
        FacadeType type = MakeFacadeType("fence", {__uuidof(ID3D12Fence)});
        Map(type, 8, &FenceGetCompletedValue);
        Map(type, 12, &FenceCompletedAddress);
        return type;
    }();
    static const FacadeType heap = [] {
        FacadeType type = MakeFacadeType("descriptor heap", {__uuidof(ID3D12DescriptorHeap)});
        Map(type, 9, &HeapCpuStart);
        Map(type, 10, &HeapGpuStart);
        return type;
    }();
    static const FacadeType resource = [] {
        FacadeType type = MakeFacadeType("resource", {__uuidof(ID3D12Resource)});
        Map(type, 8, &ResourceMap);
        Map(type, 9, &ResourceUnmap);
        Map(type, 10, &ResourceGetDesc);
        Map(type, 11, &ResourceGetGpuVirtualAddress);
        return type;
    }();
    static const FacadeType signature = MakeFacadeType("root signature", {__uuidof(ID3D12RootSignature)});
    static const FacadeType pipeline = MakeFacadeType("pipeline state", {__uuidof(ID3D12PipelineState)});
    static const FacadeType queries = MakeFacadeType("query heap", {__uuidof(ID3D12QueryHeap)});
    static const FacadeType indirect = MakeFacadeType("command signature", {__uuidof(ID3D12CommandSignature)});
    static const FacadeType range = MakeFacadeType("registered title memory", {__uuidof(ID3D12Pageable)});
    switch (kind) {
    case Kind::CommandQueue: return queue;
    case Kind::CommandAllocator: return allocator;
    case Kind::CommandList: return list;
    case Kind::Fence: return fence;
    case Kind::DescriptorHeap: return heap;
    case Kind::Resource: return resource;
    case Kind::RootSignature: return signature;
    case Kind::PipelineState: return pipeline;
    case Kind::QueryHeap: return queries;
    case Kind::CommandSignature: return indirect;
    case Kind::TitleMemoryRange: return range;
    }
    return resource;
}

}

const TitleAddressSpace* AddressSpace() {
    return g_addresses;
}

void RegisterPagePool(const void* handle, std::uint64_t start, std::uint64_t bytes) {
    const std::lock_guard guard(g_poolLock);
    g_pools[handle] = {start, bytes};
}

void SetAddressSpace(TitleAddressSpace* addresses) {
    g_addresses = addresses;
}

void SetVulkanDevice(const VulkanDevice* device) {
    g_vulkan = device;
}

void SetTimestampFrequency(UINT64 frequency) {
    g_timestampFrequency = frequency;
}

HRESULT Deliver(Kind kind, IUnknown* inner, REFIID riid, void** object) {
    if (object == nullptr) {
        inner->Release();
        return E_POINTER;
    }
    *object = nullptr;
    const FacadeType& type = TypeOf(kind);
    bool served = false;
    for (const GUID& id : type.interfaces) served = served || std::memcmp(&riid, &id, sizeof(GUID)) == 0;
    if (!served) {
        inner->Release();
        AbxUnsupported(("D3D12 " + type.name).c_str(), ("the interface " + GuidText(riid) + " is not one the console " + type.name + " facade serves").c_str());
    }
    *object = WrapInFacade(type, inner);
    return S_OK;
}

}

ABX_API const void* AbxD3D12TitleHostView(std::uint64_t address) {
    const abx::d3d12x::TitleAddressSpace* addresses = abx::d3d12x::console::AddressSpace();
    return addresses != nullptr ? addresses->HostAt(address) : nullptr;
}
