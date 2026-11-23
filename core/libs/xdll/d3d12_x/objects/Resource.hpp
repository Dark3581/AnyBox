#pragma once

#include "BufferArena.hpp"
#include "CommandRecorder.hpp"
#include "ObjectBase.hpp"
#include "TitleBufferWindow.hpp"
#include "VulkanDevice.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace abx::d3d12x {

struct ResourceContext {
    const VulkanDevice& device;
    TitleBufferWindow& window;
    BufferArena& arena;
};

constexpr std::uint64_t kBufferPlacementAlignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

class Resource final : public DeviceChildBase<ID3D12Resource> {
public:
    static HRESULT CreateCommitted(const ResourceContext& context, IUnknown* parent, const D3D12_HEAP_PROPERTIES& heap, D3D12_HEAP_FLAGS heapFlags,
                                   const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initial, REFIID riid, void** resource, std::string& error);

    HRESULT STDMETHODCALLTYPE Map(UINT subresource, const D3D12_RANGE* readRange, void** data) override;
    void STDMETHODCALLTYPE Unmap(UINT subresource, const D3D12_RANGE* writtenRange) override;
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    D3D12_RESOURCE_DESC* STDMETHODCALLTYPE GetDesc(D3D12_RESOURCE_DESC* desc) override {
        *desc = desc_;
        return desc;
    }
#else
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }
#endif
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override;
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT, const D3D12_BOX*, const void*, UINT, UINT) override;
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(void*, UINT, UINT, UINT, const D3D12_BOX*) override;
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS* flags) override;

    bool IsBuffer() const { return desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER; }
    BufferRef Buffer() const { return {context_.window.Buffer(), offset_, desc_.Width}; }
    const TextureRef& Texture() const { return texture_; }
    UINT Maps() const { return maps_; }
    VkImageView View(const VkImageViewCreateInfo& info, std::string& error);
    std::size_t Views() const;

protected:
    bool Implements(REFIID riid) const override;

private:
    Resource(const ResourceContext& context, IUnknown* parent, const D3D12_HEAP_PROPERTIES& heap, D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC& desc)
        : DeviceChildBase(parent), context_(context), heap_(heap), heapFlags_(heapFlags), desc_(desc) {}
    ~Resource() override;
    bool CreateBuffer(std::string& error);
    bool CreateTexture(D3D12_RESOURCE_STATES initial, std::string& error);

    ResourceContext context_;
    D3D12_HEAP_PROPERTIES heap_;
    D3D12_HEAP_FLAGS heapFlags_;
    D3D12_RESOURCE_DESC desc_;
    std::uint64_t offset_ = 0;
    bool allocated_ = false;
    TextureRef texture_;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    std::atomic<UINT> maps_{0};
    mutable std::mutex viewLock_;
    std::vector<std::pair<VkImageViewCreateInfo, VkImageView>> views_;
};

}
