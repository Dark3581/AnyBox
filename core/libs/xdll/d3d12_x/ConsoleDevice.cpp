#include "xdll/d3d12_x/d3d12_x.hpp"

#include "BindlessSet.hpp"
#include "CommandAllocator.hpp"
#include "CommandList.hpp"
#include "CommandQueue.hpp"
#include "CommandSignature.hpp"
#include "CopyLayout.hpp"
#include "ConsoleObjects.hpp"
#include "DescriptorHeap.hpp"
#include "DescriptorSizes.hpp"
#include "DescriptorWriter.hpp"
#include "Fence.hpp"
#include "PipelineCompiler.hpp"
#include "PipelineLibrary.hpp"
#include "PipelineObject.hpp"
#include "QueryHeap.hpp"
#include "Resource.hpp"
#include "ShaderStore.hpp"
#include "RootSignature.hpp"
#include "TextureDesc.hpp"
#include "TitleBufferWindow.hpp"
#include "VulkanDevice.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

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

struct ConsoleDevice {
    const void* const* table;
    std::atomic<ULONG> references{1};
    std::uint32_t reserved0 = 0;
    std::uint64_t reserved1 = 0;
    std::uint8_t descriptorsThroughDevice = 1;
    std::unique_ptr<VulkanDevice> vulkan;
    std::unique_ptr<abx::d3d12x::TitleBufferWindow> window;
    std::unique_ptr<abx::d3d12x::BufferArena> arena;
    std::unique_ptr<abx::d3d12x::BindlessSet> bindless;
    std::unique_ptr<abx::d3d12x::ResourceContext> resources;
    std::unique_ptr<abx::d3d12x::DescriptorWriter> descriptors;
    std::unique_ptr<abx::d3d12x::PipelineCache> pipelineCache;
    std::unique_ptr<abx::d3d12x::PipelineLibrary> pipelines;
    std::unique_ptr<abx::d3d12x::PipelineContext> pipelineContext;
    abx::d3d12x::DescriptorSizes descriptorSizes;
    abx::d3d12x::TitleAllocator titleMemory;
    std::array<std::uint32_t, kCreationParameterWords> parameters{};
    std::atomic<std::uint32_t> frameMicroseconds{0};
    std::atomic<std::uint32_t> framePeriod{0};
    std::array<std::atomic<std::int64_t>, kFrameEventKinds> frameEventOffsets{};
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
    self->frameMicroseconds = microseconds;
    self->framePeriod = period;
    AbxLog("d3d12_x", "frame interval %u us, period %u: recorded for frame pacing", microseconds, period);
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

HRESULT STDMETHODCALLTYPE DeviceCreateResourceAtAddress(ConsoleDevice* self, std::uint64_t address, const D3D12_RESOURCE_DESC* desc, std::uint32_t state, const void* clear,
                                                        REFIID riid, void** resource) {
    if (desc == nullptr || resource == nullptr) return E_INVALIDARG;
    if (clear != nullptr) {
        AbxUnsupported("D3D12 device slot 133", "a resource at a title address with an optimized clear value");
    }
    std::string error;
    IUnknown* inner = nullptr;
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

void STDMETHODCALLTYPE DeviceCreateShaderResourceViewAtAddress(ConsoleDevice* self, const D3D12_RESOURCE_DESC* resource, const std::uint8_t* view,
                                                               D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    if (resource == nullptr || view == nullptr) {
        AbxUnsupported("D3D12 device slot 70", "a null resource or view description");
    }
    if (resource->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        AbxUnsupported("D3D12 device slot 70", "a texture view at an address: its tiling and Vulkan image are not mapped yet");
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    std::memcpy(&desc, view, sizeof(desc));
    std::uint64_t address = 0;
    std::memcpy(&address, view + kViewAddressOffset, sizeof(address));
    self->descriptors->CreateShaderResourceViewAt(address, resource->Width, desc, destination);
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

UINT64 ConsoleAllocationBytes(const D3D12_RESOURCE_DESC& desc) {
    constexpr UINT kGpuWritten = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    {
        UINT64 bytes = desc.Width;
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
            if ((static_cast<UINT>(desc.Flags) & kGpuWritten) == 0) {
                AbxUnsupported("GetResourceAllocationInfo", ("a texture the title may fill itself (" + ResourceDescText(desc) +
                                                             "): its size is the console's tiled layout, which AnyBox does not compute yet")
                                                                .c_str());
            }
            const std::uint32_t mips = desc.MipLevels != 0 ? desc.MipLevels : abx::d3d12x::FullMipChain(desc.Width, desc.Height, desc.DepthOrArraySize);
            const UINT layers = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
            std::string error;
            const std::optional<abx::d3d12x::Footprints> footprints = abx::d3d12x::DesktopCopyableFootprints(desc, mips, 0, mips * layers, 0, error);
            if (!footprints) {
                AbxUnsupported("GetResourceAllocationInfo", ("a texture (" + ResourceDescText(desc) + "): " + error).c_str());
            }
            bytes = footprints->totalBytes * (std::max)(1u, desc.SampleDesc.Count);
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true)) {
                AbxLog("d3d12_x", "GetResourceAllocationInfo for GPU-written textures returns AnyBox's linear upper bound, not the console's tiled size (first: %s -> %llu bytes)",
                       ResourceDescText(desc).c_str(), static_cast<unsigned long long>(bytes));
            }
        }
        return (bytes + kAlignment - 1) / kAlignment * kAlignment;
    }
}

D3D12_RESOURCE_ALLOCATION_INFO* DeviceGetResourceAllocationInfo(D3D12_RESOURCE_ALLOCATION_INFO* info, ConsoleDevice*, UINT visibleMask, UINT count,
                                                                const D3D12_RESOURCE_DESC* descs) {
    if (visibleMask > 1 || count == 0 || descs == nullptr) {
        AbxUnsupported("GetResourceAllocationInfo", "a node mask above 1, no descriptions or a null description array");
    }
    UINT64 size = 0;
    for (UINT index = 0; index < count; ++index) {
        size += ConsoleAllocationBytes(descs[index]);
    }
    info->SizeInBytes = size;
    info->Alignment = kAlignment;
    return info;
}

constexpr UINT kDesktopResourceFlags = 0xFFFF;

std::optional<D3D12_RESOURCE_STATES> StateForLayout(std::uint32_t layout) {
    switch (layout) {
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
    table[25] = reinterpret_cast<const void*>(&DeviceGetResourceAllocationInfo);
    table[26] = reinterpret_cast<const void*>(&DeviceGetCustomHeapProperties);
    table[27] = reinterpret_cast<const void*>(&DeviceCreateCommittedResourceWithState);
    table[36] = reinterpret_cast<const void*>(&DeviceCreateFence);
    table[39] = reinterpret_cast<const void*>(&DeviceCreateQueryHeap);
    table[41] = reinterpret_cast<const void*>(&DeviceCreateCommandSignature);
    table[48] = reinterpret_cast<const void*>(&DeviceCreateConsoleQueue);
    table[62] = reinterpret_cast<const void*>(&DeviceCreateComputePipeline);
    table[70] = reinterpret_cast<const void*>(&DeviceCreateShaderResourceViewAtAddress);
    table[73] = reinterpret_cast<const void*>(&DeviceCreateConsoleCommandList);
    table[78] = reinterpret_cast<const void*>(&DeviceSetFrameInterval);
    table[79] = reinterpret_cast<const void*>(&DeviceScheduleFrameEvent);
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
    allocator.descriptorHeapAttributes = (kDescriptorHeapAllocator << 16) | kDescriptorHeapObjectType;
    allocator.committedTextureAttributes = (kDescriptorHeapAllocator << 16) | (kPageSize64KiB << 14);
    return allocator;
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
    requirements.features = {"bufferDeviceAddress"};
    requirements.hostMemoryImport = true;
    created->vulkan = VulkanDevice::Create(abx::d3d12x::DeviceRequirementsFromSettings(requirements), error);
    if (!created->vulkan) {
        AbxUnsupported(__func__, ("no usable Vulkan device: " + error).c_str());
    }
    AbxLog("d3d12_x", "D3D12XboxCreateDevice: Vulkan device %s (%s)", created->vulkan->Gpu().name.c_str(),
           abx::d3d12x::VersionText(created->vulkan->Properties().apiVersion).c_str());
    created->window = abx::d3d12x::TitleBufferWindow::Create(*created->vulkan, BufferWindowBytes(), error);
    if (!created->window) {
        AbxUnsupported(__func__, ("the GPU buffer window could not be created: " + error).c_str());
    }
    created->arena = std::make_unique<abx::d3d12x::BufferArena>(created->window->Start(), created->window->Size());
    created->titleMemory = TitleMemory();
    created->bindless = abx::d3d12x::BindlessSet::Create(*created->vulkan, error);
    if (!created->bindless) {
        AbxUnsupported(__func__, ("the bindless descriptor set could not be created: " + error).c_str());
    }
    created->resources =
        std::make_unique<abx::d3d12x::ResourceContext>(abx::d3d12x::ResourceContext{*created->vulkan, *created->window, *created->arena, &created->titleMemory, created->bindless.get()});
    created->descriptors = std::make_unique<abx::d3d12x::DescriptorWriter>(*created->resources);
    const char* cacheFile = AbxSetting("d3d12_x.pipeline_cache");
    created->pipelineCache = std::make_unique<abx::d3d12x::PipelineCache>(*created->vulkan, cacheFile != nullptr ? std::filesystem::path(cacheFile) : std::filesystem::path());
    created->pipelines = std::make_unique<abx::d3d12x::PipelineLibrary>(*created->vulkan, created->pipelineCache->Handle(), 0);
    created->pipelineContext =
        std::make_unique<abx::d3d12x::PipelineContext>(abx::d3d12x::PipelineContext{abx::d3d12x::TitleShaderStore(), *created->pipelines});
    AbxLog("d3d12_x", "D3D12XboxCreateDevice: pipeline cache %s (%zu bytes seeded)", created->pipelineCache->Note().c_str(), created->pipelineCache->SeedBytes());
    AbxLog("d3d12_x", "D3D12XboxCreateDevice: GPU buffer window %llu MiB at 0x%llx", static_cast<unsigned long long>(created->window->Size() >> 20),
           static_cast<unsigned long long>(created->window->Start()));
    g_device = created.release();
    *device = g_device;
    return S_OK;
}
