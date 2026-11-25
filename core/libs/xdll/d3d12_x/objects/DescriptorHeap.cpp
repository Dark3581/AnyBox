#include "DescriptorHeap.hpp"
#include "AbxApi.hpp"
#include "BindlessSet.hpp"
#include "DescriptorTable.hpp"

#include <algorithm>
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

const std::uint8_t* DescriptorHeap::AtGpuAddress(std::uint64_t gpu, UINT& increment) {
    HeapRegistry& registry = Registry();
    std::lock_guard<std::mutex> guard(registry.lock);
    for (const auto& [start, heap] : registry.heaps) {
        if (!heap->ShaderVisible()) continue;
        const std::uint64_t base = heap->GpuStart().ptr;
        const std::uint64_t bytes = static_cast<std::uint64_t>(heap->Count()) * heap->Increment();
        if (gpu < base || gpu - base >= bytes) continue;
        increment = heap->Increment();
        return heap->memory_ + (gpu - base);
    }
    return nullptr;
}

void DescriptorHeap::ReportForeignDescriptors() {
    HeapRegistry& registry = Registry();
    std::lock_guard<std::mutex> guard(registry.lock);
    for (const auto& [start, heap] : registry.heaps) {
        if (!heap->ShaderVisible() || heap->Type() != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) continue;
        std::size_t ours = 0, foreign = 0, samples = 0, known = 0, unknown = 0;
        for (UINT index = 0; index < heap->Count(); ++index) {
            const std::uint8_t* slot = heap->Slot(index);
            bool nonzero = false;
            for (UINT byte = 0; byte < heap->Increment() && !nonzero; ++byte) nonzero = slot[byte] != 0;
            if (heap->Record(index).kind != HeapEntry::Empty) {
                ++ours;
                continue;
            }
            if (!nonzero) continue;
            ++foreign;
            std::uint32_t words[8] = {};
            std::memcpy(words, slot, (std::min)(sizeof(words), static_cast<std::size_t>(heap->Increment())));
            int nonzeroWords = 0;
            for (const std::uint32_t word : words) nonzeroWords += word != 0;
            if (nonzeroWords < 3 || (words[3] >> 28) < 8) continue;
            const bool found = heap->context_.bindless != nullptr && heap->context_.bindless->Table().Find(ImageKey(words)).has_value();
            ++(found ? known : unknown);
            if (!found && samples++ < 8) {
                AbxLog("d3d12_x", "DIAG heap %p slot %u written by the title: %08x %08x %08x %08x %08x %08x %08x %08x", static_cast<void*>(heap->Slot(0)), index, words[0], words[1],
                       words[2], words[3], words[4], words[5], words[6], words[7]);
            }
        }
        AbxLog("d3d12_x", "DIAG heap memory %p..%p gpu 0x%llx title memory %d", static_cast<void*>(heap->memory_), static_cast<void*>(heap->memory_ + static_cast<std::size_t>(heap->Count()) * heap->Increment()),
               static_cast<unsigned long long>(heap->GpuStart().ptr), heap->titleMemory_ ? 1 : 0);
        AbxLog("d3d12_x", "DIAG heap %p (%u descriptors): %zu written by AnyBox, %zu written by the title; of those shaped like image descriptors (T# type 8-15), %zu are in the lookup table and %zu are not",
               static_cast<void*>(heap->Slot(0)), heap->Count(), ours, foreign, known, unknown);
    }
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
