#include "ConsoleObjects.hpp"

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
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

namespace abx::d3d12x::console {
namespace {

const TitleAddressSpace* g_addresses = nullptr;


std::atomic<UINT64> g_timestampFrequency{0};
std::atomic<const VulkanDevice*> g_vulkan{nullptr};

struct Label {
    std::uint64_t* address;
    std::uint64_t value;
};

std::mutex g_labelLock;
std::map<const CommandList*, std::vector<Label>> g_labels;

void STDMETHODCALLTYPE ListWriteLabel(Facade* self, std::uint64_t* address, std::uint32_t stage, std::uint64_t value, std::uint32_t mode) {
    if (address == nullptr || stage != 7 || mode != 1) {
        AbxUnsupported("D3D12 command list slot 74", "arguments other than the measured (address, 7, value, 1)");
    }
    const std::lock_guard guard(g_labelLock);
    g_labels[InnerOf<CommandList>(self)].push_back({address, value});
}

void ClearLabels(const CommandList* list) {
    const std::lock_guard guard(g_labelLock);
    g_labels.erase(list);
}

void WriteLabels(const CommandList* list) {
    std::vector<Label> labels;
    {
        const std::lock_guard guard(g_labelLock);
        const auto found = g_labels.find(list);
        if (found == g_labels.end()) return;
        labels = found->second;
    }
    for (const Label& label : labels) *label.address = label.value;
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

struct GpuPageRecord {
    std::uint64_t address;
    std::uint32_t pages;
    std::uint32_t unknown;
};

HRESULT STDMETHODCALLTYPE QueueUpdateGpuPages(Facade*, UINT count, const GpuPageRecord* records, const void* frames, UINT fifth, UINT sixth) {
    constexpr UINT kMaxRecords = 4096;
    if (count > kMaxRecords || (count != 0 && records == nullptr)) AbxUnsupported("D3D12 command queue slot 31", "more than 4096 records or a null record array");
    static std::atomic<bool> reported{false};
    if (count != 0 && !reported.exchange(true)) {
        AbxLog("d3d12_x", "command queue slot 31 updates GPU page mappings (%u records, first 0x%llx x %u pages, 0x%x; %p, 0x%x, 0x%x); AnyBox's page table already follows the CPU mappings",
               count, static_cast<unsigned long long>(records[0].address), records[0].pages, records[0].unknown, frames, fifth, sixth);
    }
    for (UINT index = 0; index < count && g_addresses != nullptr; ++index) {
        const std::uint64_t bytes = static_cast<std::uint64_t>(records[index].pages) * 0x10000;
        if (bytes != 0 && !g_addresses->Contains(records[index].address, bytes)) {
            static std::atomic<bool> missing{false};
            if (!missing.exchange(true)) {
                AbxLog("d3d12_x", "command queue slot 31: 0x%llx (%u pages) is not CPU-mapped GPU-visible memory, so shaders read zero there (later cases are not logged)",
                       static_cast<unsigned long long>(records[index].address), records[index].pages);
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
    queue->ExecuteCommandLists(count, inner);
    bool labelled = false;
    {
        const std::lock_guard guard(g_labelLock);
        for (UINT index = 0; index < count && !labelled; ++index) labelled = g_labels.count(static_cast<CommandList*>(inner[index])) != 0;
    }
    if (labelled) {
        queue->Flush();
        const VulkanDevice* device = g_vulkan.load();
        std::string error;
        if (device == nullptr || !device->Submit([](VkCommandBuffer) {}, error)) {
            AbxUnsupported("D3D12 command queue slot 35", ("waiting for the GPU before writing labels failed: " + error).c_str());
        }
        for (UINT index = 0; index < count; ++index) WriteLabels(static_cast<CommandList*>(inner[index]));
    }
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
    InnerOf<CommandQueue>(self)->Flush();
    const std::lock_guard guard(g_presentLock);
    std::string error;
    if (!g_presenter) {
        const VulkanDevice* device = g_vulkan.load();
        const HWND window = FindProcessWindow();
        if (device == nullptr || window == nullptr) AbxUnsupported("D3D12 command queue slot 32", "presenting before the title's window or the Vulkan device exists");
        g_presenter = Presenter::Create(*device, window, error);
        if (!g_presenter) AbxUnsupported("D3D12 command queue slot 32", ("the swapchain could not be created: " + error).c_str());
        AbxLog("d3d12_x", "presenting to the title's window (%u plane(s); only the first is shown)", planeCount);
    }
    const D3D12_RESOURCE_DESC& desc = image->Texture().desc;
    if (!g_presenter->Present(image->Texture().image, {static_cast<std::uint32_t>(desc.Width), desc.Height}, VK_IMAGE_LAYOUT_GENERAL, error)) {
        AbxUnsupported("D3D12 command queue slot 32", ("presenting failed: " + error).c_str());
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
}



void STDMETHODCALLTYPE ListWriteTimestamp(Facade*, std::uint64_t* address, std::uint64_t second, std::uint64_t third, std::uint32_t mode) {
    if (address == nullptr || second != 0 || third != 0 || mode != 1) {
        AbxUnsupported("D3D12 command list slot 72", "arguments other than the measured (address, 0, 0, 1)");
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
            const UINT mips = resource->Texture().layout.info.mipLevels;
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
    InnerOf<CommandList>(self)->CopyTextureRegion(&to, x, y, z, &from, box);
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
        Map(type, 70, &ListSetDepthBounds);
        Map(type, 71, &ListWriteValue);
        Map(type, 72, &ListWriteTimestamp);
        Map(type, 74, &ListWriteLabel);
        Map(type, 139, &ListReset);
        Map(type, 142, &ListSetCoverageMask);
        Map(type, 146, &ListBarrier);
        Map(type, 126, &ListSetShadingRate);
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

void SetAddressSpace(const TitleAddressSpace* addresses) {
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
