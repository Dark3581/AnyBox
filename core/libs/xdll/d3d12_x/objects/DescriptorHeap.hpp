#pragma once

#include "CommandRecorder.hpp"
#include "DescriptorSizes.hpp"
#include "ObjectBase.hpp"
#include "Resource.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace abx::d3d12x {

enum class HeapEntry : std::uint8_t { Empty, Buffer, Image, Sampler, RenderTarget, DepthStencil };

struct DescriptorRecord {
    HeapEntry kind = HeapEntry::Empty;
    bool unorderedAccess = false;
    Resource* resource = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    AttachmentRef attachment;
};

class DescriptorHeap final : public DeviceChildBase<ID3D12DescriptorHeap> {
public:
    static HRESULT Create(const ResourceContext& context, IUnknown* parent, const D3D12_DESCRIPTOR_HEAP_DESC& desc, const DescriptorSizes& sizes, REFIID riid, void** heap,
                          std::string& error);
    static DescriptorHeap* Find(SIZE_T cpu, UINT& index);

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    D3D12_DESCRIPTOR_HEAP_DESC* STDMETHODCALLTYPE GetDesc(D3D12_DESCRIPTOR_HEAP_DESC* desc) override {
        *desc = desc_;
        return desc;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE* STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart(D3D12_CPU_DESCRIPTOR_HANDLE* handle) override {
        *handle = CpuStart();
        return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE* STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart(D3D12_GPU_DESCRIPTOR_HANDLE* handle) override {
        *handle = GpuStart();
        return handle;
    }
#else
    D3D12_DESCRIPTOR_HEAP_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }
    D3D12_CPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart() override { return CpuStart(); }
    D3D12_GPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart() override { return GpuStart(); }
#endif

    D3D12_CPU_DESCRIPTOR_HANDLE CpuStart() const { return {reinterpret_cast<SIZE_T>(memory_)}; }
    D3D12_GPU_DESCRIPTOR_HANDLE GpuStart() const;
    D3D12_DESCRIPTOR_HEAP_TYPE Type() const { return desc_.Type; }
    bool ShaderVisible() const { return (desc_.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0; }
    UINT Count() const { return desc_.NumDescriptors; }
    UINT Increment() const { return increment_; }
    std::uint8_t* Slot(UINT index) const { return memory_ + static_cast<std::size_t>(index) * increment_; }
    DescriptorRecord& Record(UINT index) { return records_[index]; }
    const DescriptorRecord& Record(UINT index) const { return records_[index]; }

protected:
    bool Implements(REFIID riid) const override;

private:
    DescriptorHeap(const ResourceContext& context, IUnknown* parent, const D3D12_DESCRIPTOR_HEAP_DESC& desc, UINT increment)
        : DeviceChildBase(parent), context_(context), desc_(desc), increment_(increment), records_(desc.NumDescriptors) {}
    ~DescriptorHeap() override;
    bool Allocate(std::string& error);

    ResourceContext context_;
    D3D12_DESCRIPTOR_HEAP_DESC desc_;
    UINT increment_;
    std::uint8_t* memory_ = nullptr;
    bool titleMemory_ = false;
    std::unique_ptr<std::uint8_t[]> host_;
    std::uint64_t offset_ = 0;
    std::uint64_t gpuAlias_ = 0;
    bool allocated_ = false;
    bool registered_ = false;
    std::vector<DescriptorRecord> records_;
};

}
