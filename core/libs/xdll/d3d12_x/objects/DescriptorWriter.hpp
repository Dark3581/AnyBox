#pragma once

#include "DescriptorHeap.hpp"
#include "ImageDescriptor.hpp"
#include "SamplerDescriptor.hpp"
#include "Resource.hpp"
#include "ViewDesc.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace abx::d3d12x {

class DescriptorWriter {
public:
    explicit DescriptorWriter(const ResourceContext& context) : context_(context) {}
    ~DescriptorWriter();
    DescriptorWriter(const DescriptorWriter&) = delete;
    DescriptorWriter& operator=(const DescriptorWriter&) = delete;

    void CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination);
    void CreateShaderResourceView(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination);
    void CreateShaderResourceViewAt(std::uint64_t address, std::uint64_t bytes, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, D3D12_CPU_DESCRIPTOR_HANDLE destination);
    void CreateUnorderedAccessView(ID3D12Resource* resource, ID3D12Resource* counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination);
    void CreateRenderTargetView(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination);
    void CreateDepthStencilView(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination);
    void CreateSampler(const D3D12_SAMPLER_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination);
    void CopyDescriptors(UINT destinationRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* destinationStarts, const UINT* destinationSizes, UINT sourceRanges,
                         const D3D12_CPU_DESCRIPTOR_HANDLE* sourceStarts, const UINT* sourceSizes, D3D12_DESCRIPTOR_HEAP_TYPE type);
    void CopyDescriptorsSimple(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE destination, D3D12_CPU_DESCRIPTOR_HANDLE source, D3D12_DESCRIPTOR_HEAP_TYPE type);

    std::size_t Samplers() const;

private:
    struct Target {
        DescriptorHeap* heap = nullptr;
        UINT index = 0;
    };

    static Target Resolve(D3D12_CPU_DESCRIPTOR_HANDLE handle, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, const char* function);
    static void WriteWords(const Target& target, const void* words, std::size_t bytes);
    static void RequireImageWords(const Target& target, HeapEntry kind, const char* function);
    void WriteImageView(Resource* resource, const std::optional<ImageView>& view, bool unorderedAccess, const Target& target, const char* function, std::string& error);
    ImageDescriptorWords BindImage(Resource* resource, const VkImageViewCreateInfo& info, VkImageView view, bool unorderedAccess, const char* function);
    VkSampler SamplerFor(const D3D12_SAMPLER_DESC& desc, std::string& error);

    ResourceContext context_;
    struct BoundImage {
        ImageDescriptorWords words{};
    };
    std::mutex imageLock_;
    std::map<VkImageView, BoundImage> images_;
    std::map<std::uint32_t, std::uint32_t> nextImageSlot_;
    std::uint64_t nextImageBase_ = 1;
    std::map<VkSampler, SamplerDescriptorWords> boundSamplers_;
    std::uint32_t nextSamplerSlot_ = 1;
    mutable std::mutex samplerLock_;
    std::vector<std::pair<D3D12_SAMPLER_DESC, VkSampler>> samplers_;
};

}
