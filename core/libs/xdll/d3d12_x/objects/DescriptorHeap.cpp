#include "DescriptorHeap.hpp"

#include <cstring>
#include <iterator>
#include <map>
#include <mutex>

namespace abx::d3d12x {

namespace {

struct HeapRegistry {
    std::mutex lock;
    std::map<SIZE_T, DescriptorHeap*> heaps;
};

HeapRegistry& Registry() {
    static HeapRegistry registry;
    return registry;
}

}

HRESULT DescriptorHeap::Create(const ResourceContext& context, IUnknown* parent, const D3D12_DESCRIPTOR_HEAP_DESC& desc, const DescriptorSizes& sizes, REFIID riid, void** heap,
                               std::string& error) {
    if (heap == nullptr) {
        error = "no output for the descriptor heap";
        return E_POINTER;
    }
    *heap = nullptr;
    const std::optional<UINT> increment = DescriptorIncrement(sizes, desc.Type, error);
    if (!increment) {
        return E_INVALIDARG;
    }
    const UINT flags = static_cast<UINT>(desc.Flags);
    if ((flags & ~static_cast<UINT>(D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) != 0) {
        error = "descriptor heap flags " + std::to_string(flags) + " are unknown";
        return E_INVALIDARG;
    }
    if ((flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0 && desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
        error = "only shader resource and sampler descriptor heaps can be shader visible";
        return E_INVALIDARG;
    }
    if (desc.NodeMask > 1) {
        error = "the descriptor heap names a GPU node other than the first";
        return E_INVALIDARG;
    }
    if (!DescriptorHeapBytes(desc.NumDescriptors, *increment, error)) {
        return E_INVALIDARG;
    }
    DescriptorHeap* created = new DescriptorHeap(context, parent, desc, *increment);
    if (!created->Allocate(error)) {
        created->Release();
        return E_OUTOFMEMORY;
    }
    const HRESULT result = created->QueryInterface(riid, heap);
    created->Release();
    if (FAILED(result)) error = "the descriptor heap does not implement the requested interface";
    return result;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::GpuStart() const {
    if (!ShaderVisible()) return {0};
    if (gpuAlias_ != 0) return {gpuAlias_};
    if (titleMemory_) return {reinterpret_cast<UINT64>(memory_)};
    return {context_.window.Start() + offset_};
}

bool DescriptorHeap::Allocate(std::string& error) {
    const std::uint64_t bytes = static_cast<std::uint64_t>(desc_.NumDescriptors) * increment_;
    if (context_.titleMemory != nullptr) {
        const TitleAllocator& title = *context_.titleMemory;
        memory_ = static_cast<std::uint8_t*>(title.allocate(static_cast<std::size_t>(bytes), title.descriptorHeapAttributes));
        if (memory_ == nullptr) {
            error = "the title's allocator returned no memory for " + std::to_string(bytes) + " bytes of descriptors";
            return false;
        }
        titleMemory_ = true;
        if (ShaderVisible() && context_.addresses != nullptr) {
            const std::optional<std::uint64_t> alias = context_.addresses->AliasBelow4GiB(reinterpret_cast<std::uint64_t>(memory_), bytes, error);
            if (!alias) {
                error = "the shader-visible descriptor heap needs a GPU address below 4 GiB: " + error;
                return false;
            }
            gpuAlias_ = *alias;
        }
    } else if (ShaderVisible()) {
        const std::optional<std::uint64_t> offset = context_.arena.Allocate(bytes, kBufferPlacementAlignment);
        if (!offset) {
            error = "the buffer window has no free range of " + std::to_string(bytes) + " bytes for a shader-visible descriptor heap";
            return false;
        }
        offset_ = *offset;
        allocated_ = true;
        memory_ = context_.window.Base() + offset_;
        std::memset(memory_, 0, static_cast<std::size_t>(bytes));
    } else {
        host_ = std::make_unique<std::uint8_t[]>(static_cast<std::size_t>(bytes));
        memory_ = host_.get();
    }
    HeapRegistry& registry = Registry();
    std::lock_guard<std::mutex> guard(registry.lock);
    registry.heaps[reinterpret_cast<SIZE_T>(memory_)] = this;
    registered_ = true;
    return true;
}

DescriptorHeap::~DescriptorHeap() {
    if (registered_) {
        HeapRegistry& registry = Registry();
        std::lock_guard<std::mutex> guard(registry.lock);
        registry.heaps.erase(reinterpret_cast<SIZE_T>(memory_));
    }
    if (allocated_) context_.arena.Free(offset_);
    if (gpuAlias_ != 0 && context_.addresses != nullptr) context_.addresses->ReleaseAlias(gpuAlias_);
    if (titleMemory_) context_.titleMemory->release(memory_, context_.titleMemory->descriptorHeapAttributes);
}

bool DescriptorHeap::Implements(REFIID riid) const {
    return DeviceChildBase::Implements(riid) || riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12DescriptorHeap);
}

DescriptorHeap* DescriptorHeap::Find(SIZE_T cpu, UINT& index) {
    HeapRegistry& registry = Registry();
    std::lock_guard<std::mutex> guard(registry.lock);
    auto after = registry.heaps.upper_bound(cpu);
    if (after == registry.heaps.begin()) {
        return nullptr;
    }
    DescriptorHeap* heap = std::prev(after)->second;
    const SIZE_T distance = cpu - std::prev(after)->first;
    if (distance % heap->increment_ != 0 || distance / heap->increment_ >= heap->desc_.NumDescriptors) {
        return nullptr;
    }
    index = static_cast<UINT>(distance / heap->increment_);
    return heap;
}

}
