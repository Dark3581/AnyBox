#include "xdll/d3d12_x/d3d12_x.hpp"

#include "BindlessSet.hpp"
#include "CommandAllocator.hpp"
#include "CommandList.hpp"
#include "CommandQueue.hpp"
#include "CommandSignature.hpp"
#include "CopyLayout.hpp"
#include "ConsoleObjects.hpp"
#include "xdll/xmem/include/GpuWatch.hpp"
#include "DescriptorHeap.hpp"
#include "DescriptorSizes.hpp"
#include "DescriptorWriter.hpp"
#include "Fence.hpp"
#include "GpuProfile.hpp"
#include "GraphicsState.hpp"
#include "PipelineCompiler.hpp"
#include "PipelineLibrary.hpp"
#include "PipelineObject.hpp"
#include "PipelineStream.hpp"
#include "QueryHeap.hpp"
#include "LiveStats.hpp"
#include "Resource.hpp"
#include "ShaderStore.hpp"
#include "RootSignature.hpp"
#include "TextureDesc.hpp"
#include "TitleBufferWindow.hpp"
#include "TitleMemoryRange.hpp"
#include "HangReport.hpp"
#include "TitlePages.hpp"
#include "Gfx10Tiling.hpp"
#include "IndirectClamp.hpp"
#include "TitleTextures.hpp"
#include "TextureReadback.hpp"
#include "VulkanDevice.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#if defined(_MSC_VER)
#include <intrin.h>
#define ABX_CALLER _ReturnAddress()
#else
#define ABX_CALLER __builtin_return_address(0)
#endif

namespace {

using abx::d3d12x::DeviceRequirements;
using abx::d3d12x::GuidText;
using abx::d3d12x::VulkanDevice;
using abx::d3d12x::console::Deliver;
using abx::d3d12x::console::Kind;

constexpr std::size_t kDeviceSlots = 512;
constexpr std::size_t kCreationParameterWords = 0x50 / sizeof(std::uint32_t);
constexpr std::size_t kFrameEventKinds = 8;

constexpr GUID kConsoleDeviceInterface{0x5405c344, 0xd457, 0x444e, {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}};
constexpr GUID kUnknownInterface{0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

using WatchFunction = void (*)(AbxGpuMemoryListener, void*);

class TitleMemoryWatch {
public:
    TitleMemoryWatch(WatchFunction watch, abx::d3d12x::TitleAddressSpace* space) : watch_(watch) { watch_(&OnTitleGpuMemoryForward, space); }
    ~TitleMemoryWatch() { watch_(nullptr, nullptr); }
    TitleMemoryWatch(const TitleMemoryWatch&) = delete;
    TitleMemoryWatch& operator=(const TitleMemoryWatch&) = delete;

private:
    static void OnTitleGpuMemoryForward(void* context, std::uint32_t event, std::uint64_t start, std::uint64_t bytes, std::uint64_t source);
    WatchFunction watch_;
};

struct ConsoleDevice {
    const void* const* table;
    std::atomic<ULONG> references{1};
    std::uint32_t reserved0 = 0;
    std::uint64_t reserved1 = 0;
    std::uint8_t descriptorsThroughDevice = 1;
    std::unique_ptr<VulkanDevice> vulkan;
    std::unique_ptr<abx::d3d12x::TitleBufferWindow> window;
    std::unique_ptr<abx::d3d12x::BufferArena> arena;
    std::unique_ptr<abx::d3d12x::TitleAddressSpace> addresses;
    std::unique_ptr<TitleMemoryWatch> watch;
    std::unique_ptr<abx::d3d12x::BindlessSet> bindless;
    std::unique_ptr<abx::d3d12x::ResourceContext> resources;
    std::unique_ptr<abx::d3d12x::DescriptorWriter> descriptors;
    std::unique_ptr<abx::d3d12x::TitleTextures> titleTextures;
    std::unique_ptr<abx::d3d12x::TextureReadback> textureReadback;
    std::unique_ptr<abx::d3d12x::PipelineCache> pipelineCache;
    std::unique_ptr<abx::d3d12x::PipelineLibrary> pipelines;
    std::unique_ptr<abx::d3d12x::PipelineContext> pipelineContext;
    abx::d3d12x::DescriptorSizes descriptorSizes;
    abx::d3d12x::TitleAllocator titleMemory;
    std::array<std::uint32_t, kCreationParameterWords> parameters{};
    std::atomic<std::uint32_t> frameMicroseconds{0};
    std::atomic<std::uint32_t> framePeriod{0};
    std::array<std::atomic<std::int64_t>, kFrameEventKinds> frameEventOffsets{};
    std::mutex frameLock;
    LARGE_INTEGER frameOrigin{};
    std::unique_ptr<abx::d3d12x::TitlePages> titlePages;
    std::unique_ptr<abx::d3d12x::HangReport> hangReport;
};

static_assert(offsetof(ConsoleDevice, descriptorsThroughDevice) == 0x18);
static_assert(sizeof(D3D12_SHADER_RESOURCE_VIEW_DESC) == 0x28);

HRESULT STDMETHODCALLTYPE DeviceQueryInterface(ConsoleDevice* self, REFIID riid, void** object) {
    if (object == nullptr) return E_POINTER;
    if (std::memcmp(&riid, &kConsoleDeviceInterface, sizeof(GUID)) == 0 || std::memcmp(&riid, &kUnknownInterface, sizeof(GUID)) == 0) {
        ++self->references;
        *object = self;
        return S_OK;
    }
    *object = nullptr;
    AbxTrace("console device QueryInterface(%s): not served", GuidText(riid).c_str());
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DeviceAddRef(ConsoleDevice* self) {
    return ++self->references;
}

ULONG STDMETHODCALLTYPE DeviceRelease(ConsoleDevice* self) {
    const ULONG left = --self->references;
    if (left == 0) {
        AbxTrace("console device: last reference released, the device stays alive because title-held child objects may still reach it");
        ++self->references;
        return 0;
    }
    return left;
}

template <std::size_t Slot>
void STDMETHODCALLTYPE Unmapped(ConsoleDevice*, const void* first, const void* second, const void* third) {
    const std::string message = "slot " + std::to_string(Slot) + " of the console D3D12 device interface " + GuidText(kConsoleDeviceInterface) +
                                " is not mapped: no measured call site shows which method it is; " + abx::d3d12x::DescribeArguments(first, second, third);
    AbxUnsupported("D3D12 device", message.c_str());
}

template <std::size_t... Slots>
constexpr std::array<const void*, kDeviceSlots> UnmappedTable(std::index_sequence<Slots...>) {
    return {reinterpret_cast<const void*>(&Unmapped<Slots>)...};
}

IUnknown* AsParent(ConsoleDevice* self) {
    return reinterpret_cast<IUnknown*>(self);
}

HRESULT Report(const char* what, HRESULT result, const std::string& error) {
    if (FAILED(result)) {
        AbxLog("d3d12_x", "%s failed with 0x%08lx: %s", what, static_cast<unsigned long>(result), error.c_str());
    }
    return result;
}

HRESULT CreateQueue(ConsoleDevice* self, const D3D12_COMMAND_QUEUE_DESC& desc, REFIID riid, void** queue) {
    std::string error;
    IUnknown* inner = nullptr;
    const HRESULT result = abx::d3d12x::CommandQueue::Create(*self->vulkan, AsParent(self), desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&inner), error);
    if (FAILED(Report("CreateCommandQueue", result, error))) return result;
    return Deliver(Kind::CommandQueue, inner, riid, queue);
}

HRESULT STDMETHODCALLTYPE DeviceCreateCommandQueue(ConsoleDevice* self, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid, void** queue) {
    if (desc == nullptr || queue == nullptr) return E_INVALIDARG;
    return CreateQueue(self, *desc, riid, queue);
}

HRESULT STDMETHODCALLTYPE DeviceCreateConsoleQueue(ConsoleDevice* self, const std::uint32_t* words, REFIID riid, void** queue) {
    if (words == nullptr || queue == nullptr) return E_INVALIDARG;
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = static_cast<D3D12_COMMAND_LIST_TYPE>(words[0]);
    desc.Priority = static_cast<INT>(words[1]);
    AbxLog("d3d12_x", "console queue (type %u, priority %u): words %u and %u are recorded, not applied", words[0], words[1], words[2], words[3]);
    return CreateQueue(self, desc, riid, queue);
}

HRESULT STDMETHODCALLTYPE DeviceCreateCommandAllocator(ConsoleDevice* self, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** allocator) {
    std::string error;
    IUnknown* inner = nullptr;
    const HRESULT result =
        abx::d3d12x::CommandAllocator::Create(*self->vulkan, AsParent(self), type, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&inner), error);
    if (FAILED(Report("CreateCommandAllocator", result, error))) return result;
    return Deliver(Kind::CommandAllocator, inner, riid, allocator);
}

HRESULT STDMETHODCALLTYPE DeviceCreateConsoleCommandList(ConsoleDevice* self, const std::uint32_t* words, void* allocator, void* initial, REFIID riid, void** list) {
    if (words == nullptr || list == nullptr) return E_INVALIDARG;
    *list = nullptr;
    abx::d3d12x::CommandAllocator* inner = abx::d3d12x::console::Unwrap<abx::d3d12x::CommandAllocator>(allocator);
    if (inner == nullptr) {
        AbxUnsupported("D3D12 device slot 73", "a command allocator that AnyBox did not hand out");
    }
    if (initial != nullptr) {
        AbxUnsupported("D3D12 device slot 73", "an initial pipeline state, which has no console facade yet");
    }
    AbxTrace("console command list (type %u): words %u %u %u are recorded, not applied", words[0], words[1], words[2], words[3]);
    std::string error;
    IUnknown* created = nullptr;
    const HRESULT result = abx::d3d12x::CommandList::Create(*self->resources, AsParent(self), static_cast<D3D12_COMMAND_LIST_TYPE>(words[0]), inner, nullptr,
                                                            __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&created), error);
    if (FAILED(Report("console command list", result, error))) return result;
    return Deliver(Kind::CommandList, created, riid, list);
}

HRESULT STDMETHODCALLTYPE DeviceSetFrameInterval(ConsoleDevice* self, void* output, std::uint32_t microseconds, std::uint32_t period, std::uint32_t flags) {
    if (output != nullptr || flags != 0) {
        AbxUnsupported("D3D12 device slot 78", "a frame interval for an explicit output or with flags");
    }
    std::uint32_t applied = microseconds;
    if (const char* value = AbxSetting("d3d12_x.frame_interval_us")) {
        const unsigned long long configured = std::strtoull(value, nullptr, 10);
        if (configured >= 1000 && configured <= 1000000) applied = static_cast<std::uint32_t>(configured);
    }
    self->frameMicroseconds = applied;
    self->framePeriod = period;
    AbxLog("d3d12_x", "frame interval %u us, period %u: recorded for frame pacing%s", microseconds, period,
           applied != microseconds ? (", paced at " + std::to_string(applied) + " us instead (d3d12_x.frame_interval_us)").c_str() : "");
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceScheduleFrameEvent(ConsoleDevice* self, std::uint32_t kind, std::uint32_t offset, void* output, std::uint32_t flags) {
    if (kind >= kFrameEventKinds || output != nullptr || flags != 0) {
        AbxUnsupported("D3D12 device slot 79", "a frame event with a kind above 7, an explicit output or flags");
    }
    self->frameEventOffsets[kind] = offset;
    AbxLog("d3d12_x", "frame event kind %u at offset %u us: recorded for frame pacing", kind, offset);
    return S_OK;
}

std::optional<D3D12_RESOURCE_STATES> StateForLayout(std::uint32_t layout);
std::string ResourceDescText(const D3D12_RESOURCE_DESC& desc);

HRESULT STDMETHODCALLTYPE DeviceWaitFrameEvent(ConsoleDevice* self, std::uint32_t kind, std::uint32_t timeout, void* unused, std::uint32_t flags, std::uint64_t* token) {
    if (kind >= kFrameEventKinds || unused != nullptr || token == nullptr) {
        AbxUnsupported("D3D12 device slot 80", "a frame event kind above 7, a nonzero third argument or no token");
    }
    const std::uint32_t microseconds = self->frameMicroseconds.load();
    if (microseconds == 0) {
        AbxUnsupported("D3D12 device slot 80", "a frame event wait before the frame interval is set");
    }
    LARGE_INTEGER frequency{};
    LARGE_INTEGER now{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    std::int64_t origin = 0;
    {
        const std::lock_guard guard(self->frameLock);
        if (self->frameOrigin.QuadPart == 0) self->frameOrigin = now;
        origin = self->frameOrigin.QuadPart;
    }
    const std::int64_t offset = (std::max)(self->frameEventOffsets[kind].load(), std::int64_t{0});
    const long double tick = static_cast<long double>(frequency.QuadPart) / 1.0e6L;
    const long double interval = microseconds * tick;
    const long double since = static_cast<long double>(now.QuadPart - origin) - offset * tick;
    std::uint64_t frame = since < 0 ? 0 : static_cast<std::uint64_t>(since / interval) + 1;
    if (timeout != 0) {
        const long double target = origin + offset * tick + frame * interval;
        for (;;) {
            QueryPerformanceCounter(&now);
            const long double left = target - static_cast<long double>(now.QuadPart);
            if (left <= 0) break;
            const long double milliseconds = left / (tick * 1000.0L);
            if (milliseconds > 2.0L) Sleep(static_cast<DWORD>(milliseconds - 1.0L));
            else YieldProcessor();
        }
        ++frame;
    }
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "frame events are paced by AnyBox's clock every %u us (flags 0x%x recorded)", microseconds, flags);
    }
    *token = frame;
    return S_OK;
}

std::pair<UINT64, UINT64> ConsoleAllocation(const D3D12_RESOURCE_DESC& desc);

HRESULT STDMETHODCALLTYPE DeviceCreateResourceAtAddress(ConsoleDevice* self, std::uint64_t address, const D3D12_RESOURCE_DESC* desc, std::uint32_t state, const void* clear,
                                                        REFIID riid, void** resource) {
    constexpr UINT kGpuWritten = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    constexpr std::uint32_t kNoState = 0xFFFFFFFFu;
    if (desc == nullptr || resource == nullptr) return E_INVALIDARG;
    static_cast<void>(clear);
    std::string error;
    IUnknown* inner = nullptr;
    if (desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        const UINT flags = static_cast<UINT>(desc->Flags);
        const bool cpuFilledTarget = desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0 && desc->SampleDesc.Count <= 1 &&
                                     desc->Width <= 1024 && desc->Height <= 1024 && desc->Width * desc->Height >= 64 * 64;
        if ((flags & kGpuWritten) == 0 || cpuFilledTarget) {
            if ((flags & kGpuWritten) != 0) {
                static std::atomic<std::uint64_t> targets{0};
                const std::uint64_t count = ++targets;
                if (count <= 2 || (count & (count - 1)) == 0) {
                    AbxLog("d3d12_x", "a small render target at title address 0x%llx (%s) is kept in step with title memory, as the title fills glyph caches from the CPU (%llu so far)",
                           static_cast<unsigned long long>(address), ResourceDescText(*desc).c_str(), static_cast<unsigned long long>(count));
                }
            }
            abx::d3d12x::Resource* texture = self->titleTextures->At(address, *desc, error);
            if (texture == nullptr && error.find("no device memory") != std::string::npos) {
                static std::atomic<std::uint64_t> refused{0};
                const std::uint64_t count = ++refused;
                if (count == 1 || (count & (count - 1)) == 0) {
                    AbxLog("d3d12_x", "video memory is exhausted: device slot 133 answers E_OUTOFMEMORY and no resource for a texture at title address 0x%llx that the title "
                                      "fills itself (%s), and the title decides what to do without it (%llu so far): %s",
                           static_cast<unsigned long long>(address), ResourceDescText(*desc).c_str(), static_cast<unsigned long long>(count), error.c_str());
                }
                *resource = nullptr;
                return E_OUTOFMEMORY;
            }
            if (texture == nullptr) {
                AbxUnsupported("D3D12 device slot 133", ("a texture at a title address that the title fills itself (" + ResourceDescText(*desc) + "): " + error).c_str());
            }
            texture->AddRef();
            texture->RememberTitleAddress(address);
            return Deliver(Kind::Resource, texture, riid, resource);
        }
        const std::optional<D3D12_RESOURCE_STATES> initial = state == kNoState ? std::optional<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_COMMON) : StateForLayout(state);
        if (!initial) {
            AbxUnsupported("D3D12 device slot 133", ("initial value " + std::to_string(state) + " has no resource state AnyBox maps it to").c_str());
        }
        D3D12_RESOURCE_DESC applied = *desc;
        applied.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<UINT>(applied.Flags) & 0xFFFFu);
        applied.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        applied.Alignment = 0;
        if (abx::d3d12x::Resource* same = abx::d3d12x::Resource::GpuWrittenSame(address, applied)) {
            static std::atomic<std::uint64_t> shared{0};
            const std::uint64_t count = ++shared;
            if (count == 1 || (count & (count - 1)) == 0) {
                AbxLog("d3d12_x", "a GPU-written texture placed again at a title address where a live one with the same description sits shares it, as the two are one memory on the console (%llu so far; first at 0x%llx, %s)",
                       static_cast<unsigned long long>(count), static_cast<unsigned long long>(address), ResourceDescText(*desc).c_str());
            }
            same->AddRef();
            return Deliver(Kind::Resource, same, riid, resource);
        }
        if (abx::d3d12x::Resource* const other = abx::d3d12x::Resource::GpuWrittenAt(address, applied)) {
            static std::mutex pairLock;
            static std::set<std::pair<int, int>> pairs;
            static std::uint64_t apart = 0;
            const D3D12_RESOURCE_DESC& live = other->Texture().desc;
            const std::lock_guard pairGuard(pairLock);
            const std::uint64_t count = ++apart;
            if (pairs.insert({static_cast<int>(applied.Format), static_cast<int>(live.Format)}).second || (count & (count - 1)) == 0) {
                AbxLog("d3d12_x",
                       "a GPU-written texture is placed at title address 0x%llx (%s) where a live one of the same size sits with another description (format %d, %u mips, flags 0x%x): "
                       "it gets its own image and the two do not see each other's texels, though they are one memory on the console (%llu so far)",
                       static_cast<unsigned long long>(address), ResourceDescText(*desc).c_str(), static_cast<int>(live.Format), live.MipLevels, static_cast<unsigned>(live.Flags),
                       static_cast<unsigned long long>(count));
            }
        }
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
        const HRESULT made = abx::d3d12x::Resource::CreateCommitted(*self->resources, AsParent(self), heap, D3D12_HEAP_FLAG_NONE, applied, *initial, __uuidof(ID3D12Resource),
                                                                    reinterpret_cast<void**>(&inner), error);
        if (FAILED(made) && error.find("no device memory") != std::string::npos) {
            static std::atomic<std::uint64_t> refused{0};
            const std::uint64_t count = ++refused;
            if (count == 1 || (count & (count - 1)) == 0) {
                AbxLog("d3d12_x", "video memory is exhausted: device slot 133 answers E_OUTOFMEMORY and no resource for a GPU-written texture at title address 0x%llx (%s), and "
                                  "the title decides what to do without it (%llu so far): %s",
                       static_cast<unsigned long long>(address), ResourceDescText(*desc).c_str(), static_cast<unsigned long long>(count), error.c_str());
            }
            *resource = nullptr;
            return E_OUTOFMEMORY;
        }
        if (FAILED(made)) {
            AbxUnsupported("D3D12 device slot 133", ("a GPU-written texture at a title address (" + ResourceDescText(*desc) + "): " + error).c_str());
        }
        static_cast<abx::d3d12x::Resource*>(inner)->RememberTitleAddress(address);
        constexpr UINT kWritableStates = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_DEPTH_WRITE |
                                         D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST;
        static_cast<abx::d3d12x::Resource*>(inner)->RegisterGpuWritten(address, (static_cast<UINT>(*initial) & kWritableStates) != 0);
        self->textureReadback->Consider(*static_cast<abx::d3d12x::Resource*>(inner), address, *desc);
        const std::uint64_t vulkanBytes = static_cast<abx::d3d12x::Resource*>(inner)->TextureBytes();
        if (vulkanBytes >= (4ull << 20)) {
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 400) {
                const auto [consoleBytes, consoleAlignment] = ConsoleAllocation(applied);
                MEMORY_BASIC_INFORMATION region{};
                VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region));
                AbxLog("d3d12_x", "DIAG written 0x%llx console %llu KiB align %llu vulkan %llu KiB allocation 0x%llx region %llu KiB state 0x%lx | %s", static_cast<unsigned long long>(address),
                       static_cast<unsigned long long>(consoleBytes >> 10), static_cast<unsigned long long>(consoleAlignment), static_cast<unsigned long long>(vulkanBytes >> 10),
                       static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(region.AllocationBase)), static_cast<unsigned long long>(region.RegionSize >> 10), region.State,
                       ResourceDescText(*desc).c_str());
            }
        }
        AbxTrace("GPU-written texture at title address 0x%llx (%s): its texels live in a Vulkan image", static_cast<unsigned long long>(address),
                 ResourceDescText(*desc).c_str());
        return Deliver(Kind::Resource, inner, riid, resource);
    }
    const HRESULT result =
        abx::d3d12x::Resource::CreateAtTitleAddress(*self->resources, AsParent(self), address, *desc, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&inner), error);
    AbxTrace("resource at title address 0x%llx (dimension %d, width %llu, flags 0x%x, state 0x%x) -> 0x%08lx", static_cast<unsigned long long>(address), desc->Dimension,
             static_cast<unsigned long long>(desc->Width), desc->Flags, state, static_cast<unsigned long>(result));
    if (FAILED(Report("resource at a title address", result, error))) return result;
    return Deliver(Kind::Resource, inner, riid, resource);
}

HRESULT STDMETHODCALLTYPE DeviceCreateDescriptorHeap(ConsoleDevice* self, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** heap) {
    if (desc == nullptr || heap == nullptr) return E_INVALIDARG;
    std::string error;
    IUnknown* inner = nullptr;
    const HRESULT result = abx::d3d12x::DescriptorHeap::Create(*self->resources, AsParent(self), *desc, self->descriptorSizes, __uuidof(ID3D12DescriptorHeap),
                                                               reinterpret_cast<void**>(&inner), error);
    AbxTrace("CreateDescriptorHeap(type %d, %u descriptors, flags 0x%x) -> 0x%08lx", desc->Type, desc->NumDescriptors, desc->Flags, static_cast<unsigned long>(result));
    if (FAILED(result)) {
        AbxUnsupported("CreateDescriptorHeap", error.c_str());
    }
    return Deliver(Kind::DescriptorHeap, inner, riid, heap);
}

ID3D12Resource* TitleResource(void* resource, const char* function) {
    if (resource == nullptr) return nullptr;
    abx::d3d12x::Resource* inner = abx::d3d12x::console::Unwrap<abx::d3d12x::Resource>(resource);
    if (inner == nullptr) {
        AbxUnsupported(function, "a resource that AnyBox did not hand out");
    }
    return inner;
}

void STDMETHODCALLTYPE DeviceCreateShaderResourceView(ConsoleDevice* self, void* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    self->descriptors->CreateShaderResourceView(TitleResource(resource, "CreateShaderResourceView"), desc, destination);
}

void STDMETHODCALLTYPE DeviceCreateConstantBufferView(ConsoleDevice* self, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    self->descriptors->CreateConstantBufferView(desc, destination);
}

void STDMETHODCALLTYPE DeviceCreateUnorderedAccessView(ConsoleDevice* self, void* resource, void* counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
                                                       D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    self->descriptors->CreateUnorderedAccessView(TitleResource(resource, "CreateUnorderedAccessView"), TitleResource(counter, "CreateUnorderedAccessView"), desc, destination);
}

void STDMETHODCALLTYPE DeviceCreateRenderTargetView(ConsoleDevice* self, void* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    self->descriptors->CreateRenderTargetView(TitleResource(resource, "CreateRenderTargetView"), desc, destination);
}

void STDMETHODCALLTYPE DeviceCreateDepthStencilView(ConsoleDevice* self, void* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    self->descriptors->CreateDepthStencilView(TitleResource(resource, "CreateDepthStencilView"), desc, destination);
}

constexpr std::size_t kViewAddressOffset = sizeof(D3D12_SHADER_RESOURCE_VIEW_DESC);

void STDMETHODCALLTYPE DeviceCreateUnorderedAccessViewAtAddress(ConsoleDevice* self, const D3D12_RESOURCE_DESC* resource, const std::uint8_t* view,
                                                                 D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    const char* function = "D3D12 device slot 71 (unordered access view at an address)";
    if (resource == nullptr || view == nullptr) AbxUnsupported(function, "a null resource or view description");
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    std::memcpy(&desc, view, sizeof(desc));
    std::uint64_t address = 0;
    std::uint64_t counter = 0;
    std::memcpy(&address, view + 0x28, sizeof(address));
    std::memcpy(&counter, view + 0x30, sizeof(counter));
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "device slot 71 creates unordered access views of memory without a resource object (view desc, GPU address @0x28, counter address @0x30), as xcl reads it");
    }
    if (counter != 0) AbxUnsupported(function, "a UAV counter given by address");
    if (resource->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (self->addresses) self->addresses->MarkGpuWritten(address, resource->Width);
        self->descriptors->CreateUnorderedAccessViewAt(address, resource->Width, desc, destination);
        return;
    }
    if (abx::d3d12x::Resource* written = abx::d3d12x::Resource::GpuWrittenAt(address, *resource)) {
        self->descriptors->CreateUnorderedAccessView(written, nullptr, &desc, destination);
        return;
    }
    std::string error;
    abx::d3d12x::Resource* texture = self->titleTextures->At(address, *resource, error);
    if (texture == nullptr) {
        char text[64];
        std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
        AbxUnsupported(function, ("a texture unordered access view at title address " + std::string(text) + ": " + error).c_str());
    }
    self->descriptors->CreateUnorderedAccessView(texture, nullptr, &desc, destination);
}

void STDMETHODCALLTYPE DeviceCreateShaderResourceViewAtAddress(ConsoleDevice* self, const D3D12_RESOURCE_DESC* resource, const std::uint8_t* view,
                                                               D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    if (resource == nullptr || view == nullptr) {
        AbxUnsupported("D3D12 device slot 70", "a null resource or view description");
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    std::memcpy(&desc, view, sizeof(desc));
    std::uint64_t address = 0;
    std::memcpy(&address, view + kViewAddressOffset, sizeof(address));
    if (resource->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        std::string error;
        if (abx::d3d12x::Resource* written = abx::d3d12x::Resource::GpuWrittenAt(address, *resource)) {
            self->descriptors->CreateShaderResourceView(written, &desc, destination);
            return;
        }
        abx::d3d12x::Resource* texture = self->titleTextures->At(address, *resource, error);
        if (texture == nullptr && error.find("no device memory") != std::string::npos) {
            static std::atomic<std::uint64_t> missing{0};
            const std::uint64_t count = ++missing;
            if (count == 1 || (count & (count - 1)) == 0) {
                AbxLog("d3d12_x", "video memory is exhausted: a texture view at title address 0x%llx gets a null descriptor, so the texture is missing on screen until "
                                  "memory is free and the title creates the view again (%llu so far): %s",
                       static_cast<unsigned long long>(address), static_cast<unsigned long long>(count), error.c_str());
            }
            self->descriptors->CreateShaderResourceView(nullptr, &desc, destination);
            return;
        }
        if (texture == nullptr) {
            char text[48];
            std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
            AbxUnsupported("D3D12 device slot 70", ("a texture view at title address " + std::string(text) + ": " + error).c_str());
        }
        self->descriptors->CreateShaderResourceView(texture, &desc, destination);
        return;
    }
    self->descriptors->CreateShaderResourceViewAt(address, resource->Width, desc, destination);
}

struct ConsoleSamplerDesc {
    std::uint32_t reserved;
    std::uint32_t magFilter;
    std::uint32_t minFilter;
    std::uint32_t mipFilter;
    std::uint32_t magFilterRepeat;
    std::uint32_t reduction;
    std::uint32_t addressU;
    std::uint32_t addressV;
    std::uint32_t addressW;
    float minLod;
    float maxLod;
    std::uint32_t unknown2c;
    float mipLodBias;
    std::uint32_t maxAnisotropy;
    std::uint32_t unknown38;
    std::uint32_t unknown3c;
    std::uint32_t comparisonFunc;
    float borderColor[4];
};
static_assert(offsetof(ConsoleSamplerDesc, addressU) == 0x18 && offsetof(ConsoleSamplerDesc, mipLodBias) == 0x30 && offsetof(ConsoleSamplerDesc, comparisonFunc) == 0x40 &&
              offsetof(ConsoleSamplerDesc, borderColor) == 0x44);

void STDMETHODCALLTYPE DeviceCreateConsoleSampler(ConsoleDevice* self, const ConsoleSamplerDesc* console, D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    constexpr std::uint32_t kAnisotropic = 3;
    if (console == nullptr) {
        AbxUnsupported("D3D12 device slot 61", "a null sampler description");
    }
    const bool anisotropic = console->magFilter == kAnisotropic && console->minFilter == kAnisotropic;
    if (!anisotropic && ((console->magFilter != 1 && console->magFilter != 2) || (console->minFilter != 1 && console->minFilter != 2))) {
        AbxUnsupported("D3D12 device slot 61", "min or mag filter values other than the measured 1, 2 and 3");
    }
    if ((console->mipFilter != 1 && console->mipFilter != 2) || console->reduction > 2) {
        AbxUnsupported("D3D12 device slot 61", "mip filter or reduction values other than the measured ones");
    }
    UINT filter = 0;
    if (anisotropic) {
        filter = D3D12_ANISOTROPIC_FILTERING_BIT | (1u << D3D12_MIN_FILTER_SHIFT) | (1u << D3D12_MAG_FILTER_SHIFT);
    } else {
        filter |= (console->minFilter == 1 ? 1u : 0u) << D3D12_MIN_FILTER_SHIFT;
        filter |= (console->magFilter == 1 ? 1u : 0u) << D3D12_MAG_FILTER_SHIFT;
    }
    filter |= (console->mipFilter == 2 ? 1u : 0u) << D3D12_MIP_FILTER_SHIFT;
    const bool comparison = console->comparisonFunc > D3D12_COMPARISON_FUNC_NEVER;
    const UINT reduction = comparison                  ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON
                           : console->reduction == 1 ? D3D12_FILTER_REDUCTION_TYPE_MINIMUM
                           : console->reduction == 2 ? D3D12_FILTER_REDUCTION_TYPE_MAXIMUM
                                                     : D3D12_FILTER_REDUCTION_TYPE_STANDARD;
    filter |= reduction << D3D12_FILTER_REDUCTION_TYPE_SHIFT;
    D3D12_SAMPLER_DESC desc{};
    desc.Filter = static_cast<D3D12_FILTER>(filter);
    desc.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(console->addressU);
    desc.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(console->addressV);
    desc.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(console->addressW);
    desc.MipLODBias = console->mipLodBias;
    desc.MaxAnisotropy = anisotropic ? (std::max)(console->maxAnisotropy, 1u) : 1u;
    desc.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(console->comparisonFunc == 0 ? D3D12_COMPARISON_FUNC_NEVER : console->comparisonFunc);
    std::memcpy(desc.BorderColor, console->borderColor, sizeof(desc.BorderColor));
    desc.MinLOD = console->minLod;
    desc.MaxLOD = console->maxLod;
    self->descriptors->CreateSampler(&desc, destination);
}

void STDMETHODCALLTYPE DeviceCopyDescriptorsSimple(ConsoleDevice* self, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE destination, D3D12_CPU_DESCRIPTOR_HANDLE source,
                                                   D3D12_DESCRIPTOR_HEAP_TYPE type) {
    self->descriptors->CopyDescriptorsSimple(count, destination, source, type);
}

UINT STDMETHODCALLTYPE DeviceGetDescriptorHandleIncrementSize(ConsoleDevice* self, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    std::string error;
    const std::optional<UINT> increment = abx::d3d12x::DescriptorIncrement(self->descriptorSizes, type, error);
    if (increment) return *increment;
    static std::array<std::atomic<bool>, 8> reported{};
    const std::size_t index = (std::min)(static_cast<std::size_t>(type), reported.size() - 1);
    if (!reported[index].exchange(true)) {
        AbxLog("d3d12_x", "GetDescriptorHandleIncrementSize(%d) returns 0: %s; creating a heap of this type stops until the increment is set", type, error.c_str());
    }
    return 0;
}

HRESULT STDMETHODCALLTYPE DeviceCreateRootSignature(ConsoleDevice* self, UINT nodeMask, const void* blob, SIZE_T bytes, REFIID riid, void** signature) {
    if (nodeMask > 1) {
        AbxUnsupported("CreateRootSignature", "a node mask above 1");
    }
    std::string error;
    IUnknown* inner = nullptr;
    const HRESULT result =
        abx::d3d12x::RootSignature::Create(AsParent(self), blob, bytes, __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&inner), error);
    if (FAILED(result)) {
        AbxUnsupported("CreateRootSignature", error.c_str());
    }
    return Deliver(Kind::RootSignature, inner, riid, signature);
}

bool FromRayTracingBuilder(void* returnAddress) {
    static const HMODULE builder = GetModuleHandleW(L"XboxBvhBuilder.dll");
    HMODULE caller = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(returnAddress), &caller)) return false;
    const HMODULE current = builder != nullptr ? builder : GetModuleHandleW(L"XboxBvhBuilder.dll");
    return current != nullptr && caller == current;
}

constexpr const char* kRayTracingSkipped =
    "ray tracing acceleration structures are not built: the console builder's (XboxBvhBuilder.dll) translated compute work faulted the GPU (device lost), so its pipelines stay "
    "unbuilt, its dispatches are skipped, and scenes render without ray-traced effects";

HRESULT STDMETHODCALLTYPE DeviceCreateComputePipeline(ConsoleDevice* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* title, const void* second, const void* third,
                                                      REFIID riid, void** state) {
    if (title == nullptr || state == nullptr) return E_INVALIDARG;
    if (second != nullptr || third != nullptr) {
        AbxUnsupported("D3D12 device slot 62", "nonzero arguments after the pipeline description, which are not measured");
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = *title;
    abx::d3d12x::RootSignature* signature = abx::d3d12x::console::Unwrap<abx::d3d12x::RootSignature>(desc.pRootSignature);
    if (signature == nullptr) {
        AbxUnsupported("D3D12 device slot 62", "a root signature that AnyBox did not hand out");
    }
    desc.pRootSignature = signature;
    std::string error;
    IUnknown* inner = nullptr;
    if (FromRayTracingBuilder(ABX_CALLER)) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) AbxLog("d3d12_x", "%s", kRayTracingSkipped);
        const HRESULT skipped = abx::d3d12x::PipelineStateObject::CreateUnbuilt(AsParent(self), VK_PIPELINE_BIND_POINT_COMPUTE, signature, kRayTracingSkipped,
                                                                                __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&inner), error);
        if (FAILED(skipped)) AbxUnsupported("D3D12 device slot 62", error.c_str());
        return Deliver(Kind::PipelineState, inner, riid, state);
    }
    HRESULT result = abx::d3d12x::PipelineStateObject::CreateCompute(*self->pipelineContext, AsParent(self), desc, __uuidof(ID3D12PipelineState),
                                                                     reinterpret_cast<void**>(&inner), error);
    if (FAILED(result)) {
        if (error.find("is not in the shader store") == std::string::npos) {
            AbxUnsupported("D3D12 device slot 62", error.c_str());
        }
        AbxLog("d3d12_x", "compute pipeline without a Vulkan pipeline (binding it stops): %s", error.c_str());
        std::string reason = error;
        result = abx::d3d12x::PipelineStateObject::CreateUnbuilt(AsParent(self), VK_PIPELINE_BIND_POINT_COMPUTE, signature, std::move(reason),
                                                                 __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&inner), error);
        if (FAILED(result)) {
            AbxUnsupported("D3D12 device slot 62", error.c_str());
        }
    }
    return Deliver(Kind::PipelineState, inner, riid, state);
}

void STDMETHODCALLTYPE DeviceGetCopyableFootprints(ConsoleDevice*, const D3D12_RESOURCE_DESC* desc, UINT first, UINT count, UINT64 baseOffset,
                                                   D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, UINT* rows, UINT64* rowBytes, UINT64* totalBytes) {
    const char* function = "ID3D12Device::GetCopyableFootprints";
    if (desc == nullptr) AbxUnsupported(function, "no resource description");
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "device slot 38 is GetCopyableFootprints with the desktop arguments (first caller: a bundled module; %s, subresources %u..%u)",
               ResourceDescText(*desc).c_str(), first, first + count);
    }
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (first != 0 || count > 1) AbxUnsupported(function, "more than subresource 0 of a buffer");
        if (count == 1) {
            if (desc->Width > UINT32_MAX) AbxUnsupported(function, "a buffer of 4 GiB or more");
            if (layouts != nullptr) {
                layouts[0] = {};
                layouts[0].Offset = baseOffset;
                layouts[0].Footprint.Format = DXGI_FORMAT_UNKNOWN;
                layouts[0].Footprint.Width = static_cast<UINT>(desc->Width);
                layouts[0].Footprint.Height = 1;
                layouts[0].Footprint.Depth = 1;
                layouts[0].Footprint.RowPitch = static_cast<UINT>(desc->Width);
            }
            if (rows != nullptr) rows[0] = 1;
            if (rowBytes != nullptr) rowBytes[0] = desc->Width;
        }
        if (totalBytes != nullptr) *totalBytes = count == 1 ? desc->Width : 0;
        return;
    }
    if (desc->MipLevels == 0) AbxUnsupported(function, "a texture description with 0 mip levels (the full chain)");
    std::string error;
    const std::optional<abx::d3d12x::Footprints> footprints = abx::d3d12x::DesktopCopyableFootprints(*desc, desc->MipLevels, first, count, baseOffset, error);
    if (!footprints) AbxUnsupported(function, ("(" + ResourceDescText(*desc) + "): " + error).c_str());
    for (UINT index = 0; index < count && index < footprints->layouts.size(); ++index) {
        if (layouts != nullptr) layouts[index] = footprints->layouts[index];
        if (rows != nullptr) rows[index] = footprints->rows[index];
        if (rowBytes != nullptr) rowBytes[index] = footprints->rowBytes[index];
    }
    if (totalBytes != nullptr) *totalBytes = footprints->totalBytes;
}

HRESULT STDMETHODCALLTYPE DeviceCreateQueryHeap(ConsoleDevice* self, const D3D12_QUERY_HEAP_DESC* desc, REFIID riid, void** heap) {
    if (desc == nullptr) return E_INVALIDARG;
    std::string error;
    IUnknown* inner = nullptr;
    const HRESULT result = abx::d3d12x::QueryHeap::Create(AsParent(self), *desc, __uuidof(ID3D12QueryHeap), reinterpret_cast<void**>(&inner), error);
    if (FAILED(result)) {
        AbxUnsupported("CreateQueryHeap", error.c_str());
    }
    AbxTrace("CreateQueryHeap(type %d, %u queries)", desc->Type, desc->Count);
    return Deliver(Kind::QueryHeap, inner, riid, heap);
}

HRESULT STDMETHODCALLTYPE DeviceCreateCommandSignature(ConsoleDevice* self, const D3D12_COMMAND_SIGNATURE_DESC* desc, void* rootSignature, REFIID riid, void** signature) {
    if (desc == nullptr) return E_INVALIDARG;
    if (rootSignature != nullptr && abx::d3d12x::console::Unwrap<abx::d3d12x::RootSignature>(rootSignature) == nullptr) {
        AbxUnsupported("CreateCommandSignature", "a root signature that AnyBox did not hand out");
    }
    std::string error;
    IUnknown* inner = nullptr;
    const HRESULT result = abx::d3d12x::CommandSignature::Create(AsParent(self), *desc, __uuidof(ID3D12CommandSignature), reinterpret_cast<void**>(&inner), error);
    if (FAILED(result)) {
        AbxUnsupported("CreateCommandSignature", error.c_str());
    }
    const auto* created = static_cast<abx::d3d12x::CommandSignature*>(inner);
    AbxTrace("CreateCommandSignature(stride %u, %u arguments)%s%s", desc->ByteStride, desc->NumArgumentDescs, created->Signature() ? "" : ": executing it stops, ",
             created->Unsupported().c_str());
    return Deliver(Kind::CommandSignature, inner, riid, signature);
}

HRESULT STDMETHODCALLTYPE DeviceRegisterTitleMemory(ConsoleDevice* self, void* address, std::uint32_t pages, void** handle) {
    constexpr std::uint64_t kPageBytes = 64 * 1024;
    if (address == nullptr || pages == 0 || handle == nullptr) {
        AbxUnsupported("D3D12 device slot 74", "a null address, zero pages or no output");
    }
    const std::uint64_t start = reinterpret_cast<std::uint64_t>(address);
    const std::uint64_t bytes = static_cast<std::uint64_t>(pages) * kPageBytes;
    AbxLog("d3d12_x", "title memory 0x%llx..0x%llx (%u pages of 64 KiB) registered with the device: recorded, it has no Vulkan backing yet",
           static_cast<unsigned long long>(start), static_cast<unsigned long long>(start + bytes), pages);
    auto* range = new abx::d3d12x::TitleMemoryRange(AsParent(self), start, bytes);
    const HRESULT delivered = Deliver(Kind::TitleMemoryRange, range, __uuidof(ID3D12Pageable), handle);
    if (SUCCEEDED(delivered)) abx::d3d12x::console::RegisterPagePool(*handle, start, bytes);
    return delivered;
}

HRESULT STDMETHODCALLTYPE DeviceCreatePipelineStream(ConsoleDevice* self, const D3D12_PIPELINE_STATE_STREAM_DESC* stream, REFIID riid, void** state) {
    if (stream == nullptr || state == nullptr) return E_INVALIDARG;
    std::string error;
    std::optional<abx::d3d12x::ParsedPipelineStream> parsed = abx::d3d12x::ParsePipelineStream(stream->pPipelineStateSubobjectStream, stream->SizeInBytes, error);
    if (!parsed) {
        AbxUnsupported("D3D12 device slot 86", error.c_str());
    }
    if (parsed->depthBounds) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            AbxLog("d3d12_x", "pipeline state streams enable the depth bounds test, which AnyBox does not apply: every fragment passes it (later pipelines are not logged)");
        }
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc = parsed->graphics;
    abx::d3d12x::RootSignature* signature = abx::d3d12x::console::Unwrap<abx::d3d12x::RootSignature>(desc.pRootSignature);
    if (signature == nullptr) {
        AbxUnsupported("D3D12 device slot 86", "a pipeline state stream without a root signature AnyBox handed out");
    }
    desc.pRootSignature = signature;
    IUnknown* inner = nullptr;
    HRESULT result = E_INVALIDARG;
    VkPipelineBindPoint point = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (parsed->compute.pShaderBytecode != nullptr && FromRayTracingBuilder(ABX_CALLER)) {
        point = VK_PIPELINE_BIND_POINT_COMPUTE;
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) AbxLog("d3d12_x", "%s", kRayTracingSkipped);
        result = abx::d3d12x::PipelineStateObject::CreateUnbuilt(AsParent(self), VK_PIPELINE_BIND_POINT_COMPUTE, signature, kRayTracingSkipped, __uuidof(ID3D12PipelineState),
                                                                 reinterpret_cast<void**>(&inner), error);
        if (FAILED(result)) AbxUnsupported("D3D12 device slot 86", error.c_str());
        return Deliver(Kind::PipelineState, inner, riid, state);
    } else if (parsed->compute.pShaderBytecode != nullptr) {
        point = VK_PIPELINE_BIND_POINT_COMPUTE;
        D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
        compute.pRootSignature = signature;
        compute.CS = parsed->compute;
        compute.NodeMask = desc.NodeMask;
        compute.CachedPSO = desc.CachedPSO;
        compute.Flags = desc.Flags;
        result = abx::d3d12x::PipelineStateObject::CreateCompute(*self->pipelineContext, AsParent(self), compute, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&inner),
                                                                 error);
    } else if (parsed->mesh.pShaderBytecode != nullptr) {
        result = abx::d3d12x::PipelineStateObject::CreateMesh(*self->pipelineContext, AsParent(self), desc, parsed->mesh, parsed->amplification, __uuidof(ID3D12PipelineState),
                                                              reinterpret_cast<void**>(&inner), error);
    } else if (desc.VS.pShaderBytecode == nullptr && parsed->amplification.pShaderBytecode != nullptr) {
        const abx::d3d12x::ShaderModule* found = self->pipelineContext->store.Find(parsed->amplification.pShaderBytecode, parsed->amplification.BytecodeLength);
        if (found != nullptr && found->stage == "ms") {
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true)) {
                AbxLog("d3d12_x", "pipeline streams put a shader the shader store lists as a mesh shader in subobject 24 (amplification on the desktop) with no subobject 25; these are "
                                  "built as mesh pipelines (first: %s)",
                       found->key.c_str());
            }
            result = abx::d3d12x::PipelineStateObject::CreateMesh(*self->pipelineContext, AsParent(self), desc, parsed->amplification, D3D12_SHADER_BYTECODE{},
                                                                  __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&inner), error);
        } else if (found != nullptr && found->stage == "vs") {
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true)) {
                AbxLog("d3d12_x", "pipeline streams put a shader the shader store lists as a vertex shader in subobject 24 with no subobject 25; these are built as vertex pipelines "
                                  "(first: %s)",
                       found->key.c_str());
            }
            desc.VS = parsed->amplification;
            result = abx::d3d12x::PipelineStateObject::CreateGraphics(*self->pipelineContext, AsParent(self), desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&inner),
                                                                      error);
        } else {
            error = std::string("a shader in the amplification subobject (24) with no vertex or mesh shader (") +
                    (found != nullptr ? "the shader store lists it as stage " + found->stage + ", " + found->key : std::string("not in the shader store")) +
                    "); what the title expects of such a pipeline is not known";
        }
    } else {
        result = abx::d3d12x::PipelineStateObject::CreateGraphics(*self->pipelineContext, AsParent(self), desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&inner),
                                                                  error);
    }
    if (FAILED(result)) {
        if (error.find("is not in the shader store") == std::string::npos && error.find("amplification shaders") == std::string::npos &&
            error.find("amplification subobject") == std::string::npos) {
            AbxUnsupported("D3D12 device slot 86", ("subobjects " + parsed->types + ": " + error).c_str());
        }
        static std::atomic<std::uint64_t> unbuilt{0};
        const std::uint64_t count = ++unbuilt;
        if (count <= 8 || (count & (count - 1)) == 0) {
            AbxLog("d3d12_x", "pipeline without a Vulkan pipeline (binding it stops; %llu so far, later ones logged at powers of two): %s", static_cast<unsigned long long>(count),
                   error.c_str());
        }
        std::string reason = error;
        result = abx::d3d12x::PipelineStateObject::CreateUnbuilt(AsParent(self), point, signature, std::move(reason), __uuidof(ID3D12PipelineState),
                                                                 reinterpret_cast<void**>(&inner), error);
        if (FAILED(result)) {
            AbxUnsupported("D3D12 device slot 86", error.c_str());
        }
    }
    return Deliver(Kind::PipelineState, inner, riid, state);
}

std::string HexWords(const void* address, std::size_t words) {
    MEMORY_BASIC_INFORMATION region{};
    if (address == nullptr || VirtualQuery(address, &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 ||
        reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize - reinterpret_cast<std::uintptr_t>(address) < words * 4) {
        return "(unreadable)";
    }
    std::string text;
    for (std::size_t index = 0; index < words; ++index) {
        std::uint32_t word = 0;
        std::memcpy(&word, static_cast<const std::uint8_t*>(address) + index * 4, sizeof(word));
        char item[12];
        std::snprintf(item, sizeof(item), index == 0 ? "%08x" : " %08x", word);
        text += item;
    }
    return text;
}

struct NativeBlob {
    const void* data;
    std::uint64_t bytes;
};

std::uint64_t NativeContentHash(const NativeBlob* blobs, std::size_t count) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (std::size_t index = 0; index < count; ++index) {
        if (blobs[index].data == nullptr || blobs[index].bytes == 0 || blobs[index].bytes > (64u << 20)) continue;
        const auto* bytes = static_cast<const std::uint8_t*>(blobs[index].data);
        for (std::uint64_t at = 0; at < blobs[index].bytes; ++at) {
            hash ^= bytes[at];
            hash *= 0x100000001b3ull;
        }
        hash ^= index;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

void DumpNativeBlobs(std::uint64_t hash, const NativeBlob* blobs, std::size_t count) {
    static const bool enabled = [] {
        const char* value = AbxSetting("d3d12_x.dump_native_shaders");
        return value != nullptr && std::string_view(value) == "1";
    }();
    if (!enabled) return;
    wchar_t module[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return;
    std::filesystem::path folder = std::filesystem::path(module).parent_path() / L"native_shaders";
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(hash));
    folder /= name;
    std::error_code ignored;
    if (std::filesystem::exists(folder, ignored)) return;
    std::filesystem::create_directories(folder, ignored);
    for (std::size_t index = 0; index < count; ++index) {
        if (blobs[index].data == nullptr || blobs[index].bytes == 0 || blobs[index].bytes > (64u << 20)) continue;
        const std::filesystem::path file = folder / ("part" + std::to_string(index) + ".bin");
        const HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) continue;
        DWORD written = 0;
        WriteFile(handle, blobs[index].data, static_cast<DWORD>(blobs[index].bytes), &written, nullptr);
        CloseHandle(handle);
    }
}

HRESULT STDMETHODCALLTYPE DeviceCreateConsolePipeline(ConsoleDevice* self, void* source, std::uint32_t count, const void* descs, REFIID riid, void** state) {
    if (state == nullptr) return E_INVALIDARG;
    IUnknown* object = abx::d3d12x::FacadeInner(source);
    ID3D12RootSignature* signature = nullptr;
    auto* base = dynamic_cast<abx::d3d12x::PipelineStateObject*>(object);
    if (base != nullptr && base->Description() != nullptr && count == 1 && descs != nullptr) {
        constexpr std::uint32_t kBlendOverride = 2;
        std::uint32_t kind = 0;
        std::memcpy(&kind, descs, sizeof(kind));
        if (kind != kBlendOverride) {
            AbxUnsupported("D3D12 device slot 55", ("a pipeline variant whose override kind is not the measured blend kind 2: " + HexWords(descs, 16)).c_str());
        }
        const abx::d3d12x::GraphicsDescription& described = *base->Description();
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = described.Rebuilt();
        std::memcpy(&desc.BlendState, static_cast<const std::uint8_t*>(descs) + sizeof(kind), sizeof(desc.BlendState));
        std::string error;
        IUnknown* inner = nullptr;
        const HRESULT result = described.meshPipeline
                                   ? abx::d3d12x::PipelineStateObject::CreateMesh(*self->pipelineContext, AsParent(self), desc, described.Mesh(), described.Amplification(),
                                                                                  __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&inner), error)
                                   : abx::d3d12x::PipelineStateObject::CreateGraphics(*self->pipelineContext, AsParent(self), desc, __uuidof(ID3D12PipelineState),
                                                                                      reinterpret_cast<void**>(&inner), error);
        if (SUCCEEDED(result)) return Deliver(Kind::PipelineState, inner, riid, state);
        AbxLog("d3d12_x", "device slot 55: the blend variant could not be built (%s); it stays unbuilt", error.c_str());
    }
    if (base != nullptr) signature = base->Signature();
    if (signature == nullptr) signature = dynamic_cast<abx::d3d12x::RootSignature*>(object);
    if (signature == nullptr) {
        AbxUnsupported("D3D12 device slot 55", "a first argument that is neither a pipeline state nor a root signature AnyBox handed out");
    }
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        const std::string why = base == nullptr ? "the source is not a pipeline state"
                                : base->Description() == nullptr ? "the source pipeline has no kept description (" + (base->UnbuiltReason().empty() ? std::string("compute or native") : base->UnbuiltReason()) + ")"
                                                                 : "count " + std::to_string(count);
        AbxLog("d3d12_x", "device slot 55 pipelines stay unbuilt when they are not blend variants of a described pipeline; first case: %s; description: %s", why.c_str(),
               HexWords(descs, 16).c_str());
    }
    std::string error;
    IUnknown* inner = nullptr;
    const HRESULT result = abx::d3d12x::PipelineStateObject::CreateUnbuilt(AsParent(self), VK_PIPELINE_BIND_POINT_GRAPHICS, signature,
                                                                           "a pipeline from device slot 55, whose console description is not decoded yet", __uuidof(ID3D12PipelineState),
                                                                           reinterpret_cast<void**>(&inner), error);
    if (FAILED(result)) {
        AbxUnsupported("D3D12 device slot 55", error.c_str());
    }
    return Deliver(Kind::PipelineState, inner, riid, state);
}

HRESULT STDMETHODCALLTYPE DeviceQueryEventRecords(ConsoleDevice*, std::uint64_t token, std::uint32_t kinds, std::uint32_t* count, void* records) {
    if (count == nullptr) return E_POINTER;
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "device slot 81 (token 0x%llx, kinds 0x%x, room for %u records of 56 bytes at %p): AnyBox produces no such records and answers 0", static_cast<unsigned long long>(token),
               kinds, *count, records);
    }
    *count = 0;
    return S_OK;
}

template <VkPipelineBindPoint Point, int Slot>
HRESULT STDMETHODCALLTYPE DeviceDeserializePipelineState(ConsoleDevice*, const std::uint64_t* desc, REFIID, void** state) {
    constexpr std::size_t kPairs[][2] = {{1, 2}, {3, 4}, {5, 6}, {7, 8}, {9, 10}, {11, 12}};
    constexpr std::size_t kBlobs = Point == VK_PIPELINE_BIND_POINT_GRAPHICS ? 6 : 4;
    if (state != nullptr) *state = nullptr;
    if (desc == nullptr || state == nullptr) return E_INVALIDARG;
    NativeBlob blobs[6]{};
    for (std::size_t index = 0; index < kBlobs; ++index) blobs[index] = {reinterpret_cast<const void*>(desc[kPairs[index][0]]), desc[kPairs[index][1]]};
    DumpNativeBlobs(NativeContentHash(blobs, kBlobs), blobs, kBlobs);
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x",
               "device slot %d deserializes a %s pipeline from the console driver's pipeline cache format, which AnyBox does not decode; it answers E_NOTIMPL "
               "so the title builds the pipeline from its shaders (later calls are not logged)",
               Slot, Point == VK_PIPELINE_BIND_POINT_GRAPHICS ? "graphics" : "compute");
    }
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE DeviceCreateFence(ConsoleDevice* self, UINT64 initial, D3D12_FENCE_FLAGS flags, REFIID riid, void** fence) {
    std::string error;
    IUnknown* inner = nullptr;
    const HRESULT result = abx::d3d12x::Fence::Create(*self->vulkan, AsParent(self), initial, flags, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&inner), error);
    if (FAILED(Report("CreateFence", result, error))) return result;
    return Deliver(Kind::Fence, inner, riid, fence);
}

std::string ResourceDescText(const D3D12_RESOURCE_DESC& desc) {
    char text[256];
    std::snprintf(text, sizeof(text), "dimension %d, alignment %llu, %llux%ux%u, %u mips, format %d, %u samples, layout %d, flags 0x%x", desc.Dimension,
                  static_cast<unsigned long long>(desc.Alignment), static_cast<unsigned long long>(desc.Width), desc.Height, desc.DepthOrArraySize, desc.MipLevels, desc.Format,
                  desc.SampleDesc.Count, desc.Layout, desc.Flags);
    return text;
}

constexpr UINT64 kAlignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

std::optional<std::pair<UINT64, UINT64>> DepthStencilPlanes(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return std::pair<UINT64, UINT64>{4, 1};
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return std::pair<UINT64, UINT64>{4, 1};
    case abx::d3d12x::kConsoleD16UnormS8Uint: return std::pair<UINT64, UINT64>{2, 1};
    default: return std::nullopt;
    }
}

std::optional<UINT64> DepthStencilPlaneBytes(const D3D12_RESOURCE_DESC& desc, std::uint32_t mips) {
    constexpr UINT64 kRowAlignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    constexpr UINT64 kPlacementAlignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    const std::optional<std::pair<UINT64, UINT64>> planes = DepthStencilPlanes(desc.Format);
    if (!planes || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return std::nullopt;
    UINT64 bytes = 0;
    for (const UINT64 element : {planes->first, planes->second}) {
        for (UINT layer = 0; layer < desc.DepthOrArraySize; ++layer) {
            for (std::uint32_t mip = 0; mip < mips; ++mip) {
                const UINT64 width = (std::max)(desc.Width >> mip, UINT64{1});
                const UINT64 height = (std::max)(static_cast<UINT64>(desc.Height) >> mip, UINT64{1});
                const UINT64 pitch = (width * element + kRowAlignment - 1) / kRowAlignment * kRowAlignment;
                bytes = (bytes + kPlacementAlignment - 1) / kPlacementAlignment * kPlacementAlignment + pitch * height;
            }
        }
    }
    return bytes * (std::max)(1u, desc.SampleDesc.Count);
}

UINT64 WidestElementBound(const D3D12_RESOURCE_DESC& desc, std::uint32_t mips) {
    constexpr UINT64 kWidestElementBytes = 16;
    constexpr UINT64 kRowAlignment = 256;
    const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    UINT64 bytes = 0;
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
        const UINT64 width = (std::max)(desc.Width >> mip, UINT64{1});
        const UINT64 height = (std::max)(static_cast<UINT64>(desc.Height) >> mip, UINT64{1});
        const UINT64 depth = volume ? (std::max)(static_cast<UINT64>(desc.DepthOrArraySize) >> mip, UINT64{1}) : desc.DepthOrArraySize;
        const UINT64 pitch = (width * kWidestElementBytes + kRowAlignment - 1) / kRowAlignment * kRowAlignment;
        bytes += pitch * height * depth;
    }
    return bytes * (std::max)(1u, desc.SampleDesc.Count);
}

std::pair<UINT64, UINT64> TileElements(std::uint32_t bytes, bool volume) {
    constexpr UINT64 kWidestTileElements = 256;
    if (!volume) {
        switch (bytes) {
        case 1: return {256, 256};
        case 2: return {256, 128};
        case 4: return {128, 128};
        case 8: return {128, 64};
        case 16: return {64, 64};
        default: break;
        }
    }
    return {kWidestTileElements, kWidestTileElements};
}

UINT64 TiledUpperBound(const D3D12_RESOURCE_DESC& desc) {
    const std::optional<abx::d3d12x::CopyBlock> block = abx::d3d12x::CopyBlockOf(desc.Format);
    if (!block || block->bytes == 0) {
        AbxUnsupported("GetResourceAllocationInfo", ("a texture format with no known element size (" + ResourceDescText(desc) + ")").c_str());
    }
    const std::uint32_t mips = desc.MipLevels != 0 ? desc.MipLevels : abx::d3d12x::FullMipChain(desc.Width, desc.Height, desc.DepthOrArraySize);
    const bool volume = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    const auto [tileAcross, tileDown] = TileElements(block->bytes, volume);
    constexpr UINT64 kTileBytes = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    UINT64 bytes = 0;
    bool tail = false;
    for (std::uint32_t mip = 0; mip < mips; ++mip) {
        const UINT64 width = (std::max)(desc.Width >> mip, UINT64{1});
        const UINT64 height = (std::max)(static_cast<UINT64>(desc.Height) >> mip, UINT64{1});
        const UINT64 depth = volume ? (std::max)(static_cast<UINT64>(desc.DepthOrArraySize) >> mip, UINT64{1}) : desc.DepthOrArraySize;
        const UINT64 across = (width + block->width - 1) / block->width;
        const UINT64 down = (height + block->height - 1) / block->height;
        if (!volume && across * 2 <= tileAcross && down * 2 <= tileDown) {
            tail = true;
            break;
        }
        const UINT64 paddedAcross = (across + tileAcross - 1) / tileAcross * tileAcross;
        const UINT64 paddedDown = (down + tileDown - 1) / tileDown * tileDown;
        bytes += paddedAcross * paddedDown * depth * block->bytes;
    }
    if (tail) bytes += kTileBytes * desc.DepthOrArraySize;
    bytes *= (std::max)(1u, desc.SampleDesc.Count);
    std::string error;
    const UINT layers = volume ? 1 : desc.DepthOrArraySize;
    const std::optional<abx::d3d12x::Footprints> linear = abx::d3d12x::DesktopCopyableFootprints(desc, mips, 0, mips * layers, 0, error);
    if (linear) bytes = (std::max)(bytes, linear->totalBytes * (std::max)(1u, desc.SampleDesc.Count));
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "GetResourceAllocationInfo for textures the title may fill returns AnyBox's upper bound: the larger of AnyBox's linear layout and every mip padded to whole 64 KiB tiles (D3D12 standard tile shapes, mips within half a tile packed into one tail tile per slice; volumes padded to 256x256 elements per slice without a tail), not the console's size (first: %s -> %llu bytes)",
               ResourceDescText(desc).c_str(), static_cast<unsigned long long>(bytes));
    }
    return (bytes + kAlignment - 1) / kAlignment * kAlignment;
}

UINT64 ConsoleAllocationBytes(const D3D12_RESOURCE_DESC& desc) {
    constexpr UINT kGpuWritten = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    {
        UINT64 bytes = desc.Width;
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
            if ((static_cast<UINT>(desc.Flags) & kGpuWritten) == 0) {
                return TiledUpperBound(desc);
            }
            const std::uint32_t mips = desc.MipLevels != 0 ? desc.MipLevels : abx::d3d12x::FullMipChain(desc.Width, desc.Height, desc.DepthOrArraySize);
            const UINT layers = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
            std::string error;
            const std::optional<abx::d3d12x::Footprints> footprints = abx::d3d12x::DesktopCopyableFootprints(desc, mips, 0, mips * layers, 0, error);
            if (footprints) {
                bytes = footprints->totalBytes * (std::max)(1u, desc.SampleDesc.Count);
            } else {
                const std::optional<UINT64> planes = DepthStencilPlaneBytes(desc, mips);
                bytes = planes ? *planes : WidestElementBound(desc, mips);
            }
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true)) {
                AbxLog("d3d12_x", "GetResourceAllocationInfo for GPU-written textures returns AnyBox's linear upper bound, not the console's tiled size (first: %s -> %llu bytes)",
                       ResourceDescText(desc).c_str(), static_cast<unsigned long long>(bytes));
            }
        }
        return (bytes + kAlignment - 1) / kAlignment * kAlignment;
    }
}

std::pair<UINT64, UINT64> ConsoleAllocation(const D3D12_RESOURCE_DESC& desc) {
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        abx::d3d12x::gfx10::Refusal refusal;
        const std::optional<abx::d3d12x::gfx10::ConsoleLayout> layout = abx::d3d12x::gfx10::DescribeConsoleLayout(desc, abx::d3d12x::gfx10::ConsolePipes(AbxSetting("runtime.console_model")), refusal);
        if (layout && layout->Alignment() != 0) {
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true)) {
                AbxLog("d3d12_x", "GetResourceAllocationInfo answers textures with the console layout's size and alignment (first: %s -> %llu bytes, alignment %llu)",
                       ResourceDescText(desc).c_str(), static_cast<unsigned long long>(layout->Size()), static_cast<unsigned long long>(layout->Alignment()));
            }
            return {layout->Size(), layout->Alignment()};
        }
        static std::mutex lock;
        static std::set<std::string> reasons;
        const std::lock_guard guard(lock);
        if (reasons.insert(refusal.reason).second) {
            AbxLog("d3d12_x", "GetResourceAllocationInfo: the console layout model refuses %s (%s); AnyBox's own bound is returned for such textures", ResourceDescText(desc).c_str(),
                   refusal.reason.c_str());
        }
    }
    return {ConsoleAllocationBytes(desc), kAlignment};
}

D3D12_RESOURCE_ALLOCATION_INFO* DeviceGetResourceAllocationInfo(D3D12_RESOURCE_ALLOCATION_INFO* info, ConsoleDevice*, UINT visibleMask, UINT count,
                                                                const D3D12_RESOURCE_DESC* descs) {
    if (visibleMask > 1 || count == 0 || descs == nullptr) {
        AbxUnsupported("GetResourceAllocationInfo", "a node mask above 1, no descriptions or a null description array");
    }
    UINT64 size = 0;
    UINT64 alignment = 0;
    for (UINT index = 0; index < count; ++index) {
        const auto [bytes, align] = ConsoleAllocation(descs[index]);
        size = (size + align - 1) / align * align + bytes;
        alignment = (std::max)(alignment, align);
    }
    info->SizeInBytes = size;
    info->Alignment = alignment;
    return info;
}

constexpr UINT kDesktopResourceFlags = 0xFFFF;

std::optional<D3D12_RESOURCE_STATES> StateForLayout(std::uint32_t layout) {
    switch (layout) {
    case static_cast<std::uint32_t>(D3D12_BARRIER_LAYOUT_UNDEFINED):
    case 0: return D3D12_RESOURCE_STATE_COMMON;
    case 1: return D3D12_RESOURCE_STATE_GENERIC_READ;
    case 2: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case 3: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case 4: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case 5: return D3D12_RESOURCE_STATE_DEPTH_READ;
    case 6: return D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    case 7: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case 8: return D3D12_RESOURCE_STATE_COPY_DEST;
    default: return std::nullopt;
    }
}

HRESULT STDMETHODCALLTYPE DeviceCreateCommittedResourceWithState(ConsoleDevice* self, const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS heapFlags,
                                                                 const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initial, const D3D12_CLEAR_VALUE* clear, REFIID riid,
                                                                 void** resource) {
    static_cast<void>(clear);
    if (heap == nullptr || desc == nullptr || resource == nullptr) return E_INVALIDARG;
    D3D12_RESOURCE_DESC applied = *desc;
    applied.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<UINT>(applied.Flags) & kDesktopResourceFlags);
    if (applied.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && applied.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            AbxLog("d3d12_x", "a committed texture that asks for texture layout %d is stored in the driver's own layout: its memory has no title address, so the layout "
                              "cannot be seen through memory (first: %s)",
                   static_cast<int>(applied.Layout), ResourceDescText(*desc).c_str());
        }
        applied.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }
    std::string error;
    IUnknown* inner = nullptr;
    const HRESULT result = abx::d3d12x::Resource::CreateCommitted(*self->resources, AsParent(self), *heap, heapFlags, applied, initial, __uuidof(ID3D12Resource),
                                                                  reinterpret_cast<void**>(&inner), error);
    AbxTrace("CreateCommittedResource (%s, heap type %d) -> 0x%08lx", ResourceDescText(*desc).c_str(), heap->Type, static_cast<unsigned long>(result));
    if (FAILED(result)) {
        AbxUnsupported("CreateCommittedResource", ("(" + ResourceDescText(*desc) + "): " + error).c_str());
    }
    return Deliver(Kind::Resource, inner, riid, resource);
}

HRESULT STDMETHODCALLTYPE DeviceCreateCommittedResource(ConsoleDevice* self, const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC* desc,
                                                        std::uint32_t initial, const D3D12_CLEAR_VALUE* clear, void* session, std::uint32_t castableCount,
                                                        const DXGI_FORMAT* castable, REFIID riid, void** resource) {
    static_cast<void>(clear);
    if (heap == nullptr || desc == nullptr || resource == nullptr) return E_INVALIDARG;
    if (session != nullptr || castableCount != 0 || castable != nullptr) {
        AbxUnsupported("D3D12 device slot 127", "a protected session or castable formats");
    }
    const std::optional<D3D12_RESOURCE_STATES> state = StateForLayout(initial);
    if (!state) {
        AbxUnsupported("D3D12 device slot 127", ("initial barrier layout " + std::to_string(initial) + " has no resource state AnyBox maps it to").c_str());
    }
    D3D12_RESOURCE_DESC applied = *desc;
    const UINT consoleFlags = static_cast<UINT>(applied.Flags) & ~kDesktopResourceFlags;
    applied.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<UINT>(applied.Flags) & kDesktopResourceFlags);
    if (consoleFlags != 0) {
        AbxTrace("committed resource: console-only resource flags 0x%x recorded, not applied", consoleFlags);
    }
    std::string error;
    IUnknown* inner = nullptr;
    const HRESULT result = abx::d3d12x::Resource::CreateCommitted(*self->resources, AsParent(self), *heap, heapFlags, applied, *state,
                                                                  __uuidof(ID3D12Resource), reinterpret_cast<void**>(&inner), error);
    AbxTrace("committed resource (%s, heap type %d, heap flags 0x%x) -> 0x%08lx", ResourceDescText(*desc).c_str(), heap->Type, heapFlags, static_cast<unsigned long>(result));
    if (FAILED(result)) {
        AbxUnsupported("D3D12 device slot 127", ("a committed resource AnyBox cannot create (" + ResourceDescText(*desc) + "): " + error).c_str());
    }
    if (desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        const UINT64 bytes = ConsoleAllocationBytes(applied);
        void* const memory = self->titleMemory.allocate(static_cast<std::size_t>(bytes), self->titleMemory.committedTextureAttributes);
        if (memory == nullptr) {
            AbxUnsupported("D3D12 device slot 127", "the title's allocator returned no memory for a committed texture");
        }
        static_cast<abx::d3d12x::Resource*>(inner)->AttachTitleMemory(memory, self->titleMemory.committedTextureAttributes);
        AbxTrace("committed texture: %llu bytes taken from the title's allocator at %p (texels live in the Vulkan image)", static_cast<unsigned long long>(bytes), memory);
    }
    return Deliver(Kind::Resource, inner, riid, resource);
}

D3D12_RESOURCE_ALLOCATION_INFO* DeviceGetResourceAllocationInfo1(D3D12_RESOURCE_ALLOCATION_INFO* info, ConsoleDevice*, UINT visibleMask, UINT count,
                                                                 const D3D12_RESOURCE_DESC* descs, D3D12_RESOURCE_ALLOCATION_INFO1* each) {
    if (visibleMask > 1 || count == 0 || descs == nullptr) {
        AbxUnsupported("GetResourceAllocationInfo1", "a node mask above 1, no descriptions or a null description array");
    }
    UINT64 size = 0;
    UINT64 alignment = 0;
    for (UINT index = 0; index < count; ++index) {
        const auto [bytes, align] = ConsoleAllocation(descs[index]);
        const UINT64 offset = (size + align - 1) / align * align;
        if (each != nullptr) each[index] = {offset, align, bytes};
        size = offset + bytes;
        alignment = (std::max)(alignment, align);
    }
    info->SizeInBytes = size;
    info->Alignment = alignment;
    return info;
}

D3D12_HEAP_PROPERTIES* DeviceGetCustomHeapProperties(D3D12_HEAP_PROPERTIES* properties, ConsoleDevice*, UINT nodeMask, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES result{};
    result.Type = D3D12_HEAP_TYPE_CUSTOM;
    result.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    result.CreationNodeMask = nodeMask;
    result.VisibleNodeMask = nodeMask;
    switch (type) {
    case D3D12_HEAP_TYPE_DEFAULT: result.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE; break;
    case D3D12_HEAP_TYPE_UPLOAD: result.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE; break;
    case D3D12_HEAP_TYPE_READBACK: result.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK; break;
    default: AbxUnsupported("GetCustomHeapProperties", ("heap type " + std::to_string(type)).c_str());
    }
    *properties = result;
    return properties;
}

std::array<const void*, kDeviceSlots> DeviceTable() {
    std::array<const void*, kDeviceSlots> table = UnmappedTable(std::make_index_sequence<kDeviceSlots>{});
    table[0] = reinterpret_cast<const void*>(&DeviceQueryInterface);
    table[1] = reinterpret_cast<const void*>(&DeviceAddRef);
    table[2] = reinterpret_cast<const void*>(&DeviceRelease);
    table[8] = reinterpret_cast<const void*>(&DeviceCreateCommandQueue);
    table[9] = reinterpret_cast<const void*>(&DeviceCreateCommandAllocator);
    table[14] = reinterpret_cast<const void*>(&DeviceCreateDescriptorHeap);
    table[15] = reinterpret_cast<const void*>(&DeviceGetDescriptorHandleIncrementSize);
    table[16] = reinterpret_cast<const void*>(&DeviceCreateRootSignature);
    table[17] = reinterpret_cast<const void*>(&DeviceCreateConstantBufferView);
    table[18] = reinterpret_cast<const void*>(&DeviceCreateShaderResourceView);
    table[19] = reinterpret_cast<const void*>(&DeviceCreateUnorderedAccessView);
    table[20] = reinterpret_cast<const void*>(&DeviceCreateRenderTargetView);
    table[21] = reinterpret_cast<const void*>(&DeviceCreateDepthStencilView);
    table[24] = reinterpret_cast<const void*>(&DeviceCopyDescriptorsSimple);
    table[25] = reinterpret_cast<const void*>(&DeviceGetResourceAllocationInfo);
    table[26] = reinterpret_cast<const void*>(&DeviceGetCustomHeapProperties);
    table[27] = reinterpret_cast<const void*>(&DeviceCreateCommittedResourceWithState);
    table[36] = reinterpret_cast<const void*>(&DeviceCreateFence);
    table[38] = reinterpret_cast<const void*>(&DeviceGetCopyableFootprints);
    table[39] = reinterpret_cast<const void*>(&DeviceCreateQueryHeap);
    table[41] = reinterpret_cast<const void*>(&DeviceCreateCommandSignature);
    table[48] = reinterpret_cast<const void*>(&DeviceCreateConsoleQueue);
    table[61] = reinterpret_cast<const void*>(&DeviceCreateConsoleSampler);
    table[62] = reinterpret_cast<const void*>(&DeviceCreateComputePipeline);
    table[55] = reinterpret_cast<const void*>(&DeviceCreateConsolePipeline);
    table[57] = reinterpret_cast<const void*>(&DeviceDeserializePipelineState<VK_PIPELINE_BIND_POINT_GRAPHICS, 57>);
    table[67] = reinterpret_cast<const void*>(&DeviceDeserializePipelineState<VK_PIPELINE_BIND_POINT_COMPUTE, 67>);
    table[70] = reinterpret_cast<const void*>(&DeviceCreateShaderResourceViewAtAddress);
    table[71] = reinterpret_cast<const void*>(&DeviceCreateUnorderedAccessViewAtAddress);
    table[73] = reinterpret_cast<const void*>(&DeviceCreateConsoleCommandList);
    table[74] = reinterpret_cast<const void*>(&DeviceRegisterTitleMemory);
    table[76] = reinterpret_cast<const void*>(&DeviceCreateCommittedResourceWithState);
    table[78] = reinterpret_cast<const void*>(&DeviceSetFrameInterval);
    table[80] = reinterpret_cast<const void*>(&DeviceWaitFrameEvent);
    table[81] = reinterpret_cast<const void*>(&DeviceQueryEventRecords);
    table[86] = reinterpret_cast<const void*>(&DeviceCreatePipelineStream);
    table[79] = reinterpret_cast<const void*>(&DeviceScheduleFrameEvent);
    table[108] = reinterpret_cast<const void*>(&DeviceGetResourceAllocationInfo1);
    table[127] = reinterpret_cast<const void*>(&DeviceCreateCommittedResource);
    table[133] = reinterpret_cast<const void*>(&DeviceCreateResourceAtAddress);
    return table;
}

const std::array<const void*, kDeviceSlots> kDeviceTable = DeviceTable();

std::uint64_t BufferWindowBytes() {
    constexpr std::uint64_t kDefaultMebibytes = 512;
    const char* text = AbxSetting("d3d12_x.buffer_window_mib");
    std::uint64_t mebibytes = kDefaultMebibytes;
    if (text != nullptr) {
        char* end = nullptr;
        mebibytes = std::strtoull(text, &end, 10);
        if (end == text || *end != '\0' || mebibytes == 0 || mebibytes > 3072) {
            AbxUnsupported("D3D12XboxCreateDevice", "d3d12_x.buffer_window_mib must be a whole number of MiB from 1 to 3072");
        }
    }
    return mebibytes << 20;
}

constexpr std::uint64_t kDescriptorHeapAllocator = 0x80;
constexpr std::uint64_t kDescriptorHeapObjectType = 4;
constexpr std::uint64_t kPageSize64KiB = 2;
constexpr std::uint64_t kAttributePageSize64KiB = 1;
constexpr std::uint64_t kAttributeAlignment64KiB = 16;
constexpr std::uint64_t kAttributeGraphicsMemory = 1;

std::atomic<abx::d3d12x::TitleTextures*> g_titleTextures{nullptr};

void OnTitleGpuMemory(void* context, std::uint32_t event, std::uint64_t start, std::uint64_t bytes, std::uint64_t source) {
    auto* space = static_cast<abx::d3d12x::TitleAddressSpace*>(context);
    std::string error;
    switch (event) {
    case kAbxGpuMemoryWithdrawn: space->Release(start, bytes); return;
    case kAbxGpuMemoryBackingAvailable: space->AddBacking(start, bytes); return;
    case kAbxGpuMemoryBackingWithdrawn: space->RemoveBacking(start, bytes); return;
    case kAbxGpuMemoryAliased:
        if (!space->AliasPhysical(start, bytes, source, error)) {
            AbxLog("d3d12_x", "title memory 0x%llx..0x%llx stays invisible to the GPU (reads there return zero): %s", static_cast<unsigned long long>(start),
                   static_cast<unsigned long long>(start + bytes), error.c_str());
        }
        return;
    case kAbxGpuMemoryWritten:
        if (abx::d3d12x::TitleTextures* textures = g_titleTextures.load()) textures->Written(start, bytes);
        return;
    case kAbxGpuMemoryAvailable: break;
    default: AbxUnsupported("d3d12_x title memory watch", ("GPU memory event " + std::to_string(event)).c_str());
    }
    if (!space->Import(start, bytes, error)) {
        AbxLog("d3d12_x", "title memory 0x%llx..0x%llx stays invisible to the GPU (reads there return zero): %s", static_cast<unsigned long long>(start),
               static_cast<unsigned long long>(start + bytes), error.c_str());
    }
}

abx::d3d12x::TitleAllocator TitleMemory() {
    abx::d3d12x::TitleAllocator allocator;
    const HMODULE xmem = GetModuleHandleW(L"ab_xmem.dll");
    if (xmem == nullptr) {
        AbxUnsupported("D3D12XboxCreateDevice", "ab_xmem.dll is not loaded, so descriptor heap memory cannot come from the title's allocator");
    }
    allocator.allocate = reinterpret_cast<void* (*)(std::size_t, std::uint64_t)>(reinterpret_cast<void*>(GetProcAddress(xmem, "XMemAlloc")));
    allocator.release = reinterpret_cast<void (*)(void*, std::uint64_t)>(reinterpret_cast<void*>(GetProcAddress(xmem, "XMemFree")));
    if (allocator.allocate == nullptr || allocator.release == nullptr) {
        AbxUnsupported("D3D12XboxCreateDevice", "ab_xmem.dll does not export XMemAlloc and XMemFree");
    }
    allocator.descriptorHeapAttributes = kDescriptorHeapObjectType | (kAttributePageSize64KiB << 14) | (kDescriptorHeapAllocator << 16) | (kAttributeAlignment64KiB << 24) |
                                         (kAttributeGraphicsMemory << 29);
    allocator.committedTextureAttributes = (kDescriptorHeapAllocator << 16) | (kPageSize64KiB << 14);
    return allocator;
}

void TitleMemoryWatch::OnTitleGpuMemoryForward(void* context, std::uint32_t event, std::uint64_t start, std::uint64_t bytes, std::uint64_t source) {
    OnTitleGpuMemory(context, event, start, bytes, source);
}

void ApplyGpuPriority() {
    const char* value = AbxSetting("d3d12_x.gpu_priority");
    if (value == nullptr || value[0] == 0) return;
    const std::string asked(value);
    const int priority = asked == "idle" ? 0 : asked == "below_normal" ? 1 : asked == "normal" ? 2 : -1;
    if (priority < 0) {
        AbxLog("d3d12_x", "d3d12_x.gpu_priority = %s is not idle, below_normal or normal; the GPU scheduling priority is left as it is", value);
        return;
    }
    using SetPriorityClass = LONG(WINAPI*)(HANDLE, int);
    const HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    const auto set = gdi != nullptr ? reinterpret_cast<SetPriorityClass>(reinterpret_cast<void*>(GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass"))) : nullptr;
    if (set == nullptr) {
        AbxLog("d3d12_x", "d3d12_x.gpu_priority: gdi32.dll has no D3DKMTSetProcessSchedulingPriorityClass; the GPU scheduling priority is left as it is");
        return;
    }
    const LONG status = set(GetCurrentProcess(), priority);
    AbxLog("d3d12_x", "GPU scheduling priority class of this process: %s (d3d12_x.gpu_priority; the system answered 0x%08lx), so the desktop and other programs get the GPU before "
                      "the title's work",
           value, static_cast<unsigned long>(status));
}

std::mutex g_deviceLock;
ConsoleDevice* g_device = nullptr;

}

ABX_API HRESULT D3D12XboxCreateDevice(IUnknown* adapter, const void* parameters, REFIID riid, void** device) {
    if (device == nullptr) return E_POINTER;
    *device = nullptr;
    if (adapter != nullptr) {
        AbxUnsupported(__func__, "an explicit adapter");
    }
    if (parameters == nullptr) {
        AbxUnsupported(__func__, "null creation parameters");
    }
    if (std::memcmp(&riid, &kConsoleDeviceInterface, sizeof(GUID)) != 0) {
        AbxUnsupported(__func__, ("a device interface other than " + GuidText(kConsoleDeviceInterface) + ": " + GuidText(riid)).c_str());
    }
    const std::lock_guard guard(g_deviceLock);
    if (g_device != nullptr) {
        AbxUnsupported(__func__, "a second device");
    }
    auto created = std::make_unique<ConsoleDevice>();
    created->table = kDeviceTable.data();
    for (auto& offset : created->frameEventOffsets) offset = -1;
    std::memcpy(created->parameters.data(), parameters, sizeof(created->parameters));
    std::string error;
    const std::optional<abx::d3d12x::DescriptorSizes> sizes = abx::d3d12x::DescriptorSizesFromSettings(error);
    if (!sizes) {
        AbxUnsupported(__func__, error.c_str());
    }
    created->descriptorSizes = *sizes;
    const auto& words = created->parameters;
    AbxLog("d3d12_x",
           "D3D12XboxCreateDevice parameters %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x: recorded, their meaning "
           "is not measured yet",
           words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7], words[8], words[9], words[10], words[11], words[12], words[13], words[14],
           words[15], words[16], words[17], words[18], words[19]);
    DeviceRequirements requirements;
    requirements.features = {"bufferDeviceAddress", "meshShader"};
    requirements.hostMemoryImport = true;
    requirements.presentation = true;
    created->vulkan = VulkanDevice::Create(abx::d3d12x::DeviceRequirementsFromSettings(requirements), error);
    if (!created->vulkan) {
        AbxUnsupported(__func__, ("no usable Vulkan device: " + error).c_str());
    }
    const float period = created->vulkan->Properties().limits.timestampPeriod;
    if (period > 0.0f) abx::d3d12x::console::SetTimestampFrequency(static_cast<UINT64>(1.0e9 / period));
    abx::d3d12x::console::SetVulkanDevice(created->vulkan.get());
    abx::d3d12x::GpuProfile::Start(*created->vulkan);
    ApplyGpuPriority();
    AbxLog("d3d12_x", "D3D12XboxCreateDevice: Vulkan device %s (%s)", created->vulkan->Gpu().name.c_str(),
           abx::d3d12x::VersionText(created->vulkan->Properties().apiVersion).c_str());
    AbxLog("d3d12_x",
           created->vulkan->PageableMemory()
               ? "video memory is pageable (VK_EXT_pageable_device_local_memory with VK_EXT_memory_priority): allocations past the card's own memory succeed and the system moves "
                 "what is used least into system memory, so running out of video memory costs speed instead of failing (d3d12_x.pageable_video_memory: 0 turns it off, 2 also "
                 "gives GPU-written textures priority 0.75 instead of 0.5)"
               : "video memory is not pageable (d3d12_x.pageable_video_memory is 0, or the device lacks VK_EXT_pageable_device_local_memory or VK_EXT_memory_priority): allocations "
                 "past the card's own memory fail");
    created->window = abx::d3d12x::TitleBufferWindow::Create(*created->vulkan, BufferWindowBytes(), error);
    if (!created->window) {
        AbxUnsupported(__func__, ("the GPU buffer window could not be created: " + error).c_str());
    }
    created->arena = std::make_unique<abx::d3d12x::BufferArena>(created->window->Start(), created->window->Size());
    created->addresses = abx::d3d12x::TitleAddressSpace::Create(*created->vulkan, error);
    if (!created->addresses || !created->addresses->MapDevice(created->window->Start(), created->window->Size(), created->window->DeviceAddress(), created->window->Buffer(), error)) {
        AbxUnsupported(__func__, ("the title address space could not be created: " + error).c_str());
    }
    abx::d3d12x::console::SetAddressSpace(created->addresses.get());
    created->vulkan->SetFaultAddressDescriber([space = created->addresses.get()](std::uint64_t device) { return space->DescribeDeviceAddress(device); });
    created->vulkan->SetFaultExtraReport([] { return abx::d3d12x::IndirectClamp::RecentReport(); });
    {
        const HMODULE xmem = GetModuleHandleW(L"ab_xmem.dll");
        const auto watch = xmem != nullptr ? reinterpret_cast<WatchFunction>(reinterpret_cast<void*>(GetProcAddress(xmem, "AbxXMemWatchGpuMemory"))) : nullptr;
        if (watch == nullptr) {
            AbxUnsupported(__func__, "ab_xmem.dll does not export AbxXMemWatchGpuMemory, so title memory cannot reach the GPU");
        }
        created->titlePages = abx::d3d12x::TitlePages::Start(*created->addresses);
        created->hangReport = abx::d3d12x::HangReport::Start(*created->vulkan, created->addresses.get());
        created->watch = std::make_unique<TitleMemoryWatch>(watch, created->addresses.get());
    }
    AbxLog("d3d12_x", "title memory reaches shaders through a page table at 0x%llx; %llu MiB of GPU-visible title memory imported so far",
           static_cast<unsigned long long>(created->addresses->RootTable()), static_cast<unsigned long long>(created->addresses->ImportedBytes() >> 20));
    created->titleMemory = TitleMemory();
    created->bindless = abx::d3d12x::BindlessSet::Create(*created->vulkan, error);
    if (!created->bindless) {
        AbxUnsupported(__func__, ("the bindless descriptor set could not be created: " + error).c_str());
    }
    created->resources =
        std::make_unique<abx::d3d12x::ResourceContext>(abx::d3d12x::ResourceContext{*created->vulkan, *created->window, *created->arena, &created->titleMemory, created->bindless.get(), created->addresses.get()});
    created->descriptors = std::make_unique<abx::d3d12x::DescriptorWriter>(*created->resources);
    created->titleTextures = std::make_unique<abx::d3d12x::TitleTextures>(*created->resources, AsParent(created.get()));
    g_titleTextures.store(created->titleTextures.get());
    created->textureReadback = std::make_unique<abx::d3d12x::TextureReadback>(*created->resources);
    const char* cacheFile = AbxSetting("d3d12_x.pipeline_cache");
    created->pipelineCache = std::make_unique<abx::d3d12x::PipelineCache>(*created->vulkan, cacheFile != nullptr ? std::filesystem::path(cacheFile) : std::filesystem::path());
    created->pipelines = std::make_unique<abx::d3d12x::PipelineLibrary>(*created->vulkan, created->pipelineCache->Handle(), 0);
    created->pipelineContext =
        std::make_unique<abx::d3d12x::PipelineContext>(abx::d3d12x::PipelineContext{abx::d3d12x::TitleShaderStore(), *created->pipelines});
    AbxLog("d3d12_x", "D3D12XboxCreateDevice: pipeline cache %s (%zu bytes seeded)", created->pipelineCache->Note().c_str(), created->pipelineCache->SeedBytes());
    if (cacheFile != nullptr && cacheFile[0] != 0) {
        const abx::d3d12x::PipelineCache* cache = created->pipelineCache.get();
        std::thread([cache] {
            constexpr std::uint64_t kNewPipelines = 50;
            constexpr int kQuietChecks = 20;
            constexpr ULONGLONG kFreeCommit = 4ull << 30;
            constexpr ULONGLONG kSaveApartMs = 120000;
            std::uint64_t saved = abx::d3d12x::Stats().pipelinesBuilt.load();
            std::uint64_t savedCompiled = saved - abx::d3d12x::Stats().pipelinesFromCache.load();
            ULONGLONG savedAt = 0;
            bool first = true;
            int quiet = 0;
            for (;;) {
                Sleep(15000);
                const std::uint64_t built = abx::d3d12x::Stats().pipelinesBuilt.load();
                if (built == saved) continue;
                const std::uint64_t fromCache = abx::d3d12x::Stats().pipelinesFromCache.load();
                const std::uint64_t compiled = built > fromCache ? built - fromCache : 0;
                if (compiled == savedCompiled) {
                    static std::atomic<bool> reported{false};
                    if (!reported.exchange(true)) {
                        AbxLog("d3d12_x", "the pipeline cache is not saved again while every pipeline built since the last save came out of it (first: %llu built, none compiled)",
                               static_cast<unsigned long long>(built));
                    }
                    saved = built;
                    quiet = 0;
                    continue;
                }
                if (savedAt != 0 && GetTickCount64() - savedAt < kSaveApartMs) continue;
                ++quiet;
                const bool settled = abx::d3d12x::Stats().pipelinesQueued.load() == 0 && abx::d3d12x::Stats().pipelinesBuilding.load() == 0;
                if (built - saved < kNewPipelines && quiet < kQuietChecks && !settled) continue;
                if (first && !settled && quiet < 4) continue;
                MEMORYSTATUSEX memory{sizeof(memory)};
                if (GlobalMemoryStatusEx(&memory) && memory.ullAvailPageFile < kFreeCommit) continue;
                std::size_t written = 0;
                std::string failure;
                const auto begin = std::chrono::steady_clock::now();
                const bool ok = cache->Save(written, failure);
                const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
                if (ok) {
                    AbxLog("d3d12_x", "pipeline cache saved: %zu MiB for %llu pipelines in %lld ms (saved again as pipelines are built, so later runs do not compile them again)",
                           written >> 20, static_cast<unsigned long long>(built), static_cast<long long>(took));
                    saved = built;
                    savedCompiled = compiled;
                    savedAt = GetTickCount64();
                    first = false;
                    quiet = 0;
                } else {
                    AbxLog("d3d12_x", "pipeline cache not saved: %s", failure.c_str());
                    quiet = 0;
                }
            }
        }).detach();
    }
    AbxLog("d3d12_x", "D3D12XboxCreateDevice: GPU buffer window %llu MiB at 0x%llx", static_cast<unsigned long long>(created->window->Size() >> 20),
           static_cast<unsigned long long>(created->window->Start()));
    g_device = created.release();
    *device = g_device;
    return S_OK;
}
