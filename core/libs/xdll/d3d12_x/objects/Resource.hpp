#pragma once

#include "BindlessSet.hpp"
#include "BufferArena.hpp"
#include "CommandRecorder.hpp"
#include "DepthPlanePass.hpp"
#include "ObjectBase.hpp"
#include "TitleAddressSpace.hpp"
#include "TitleBufferWindow.hpp"
#include "VulkanDevice.hpp"

#include <optional>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace abx::d3d12x {

struct TitleAllocator {
    void* (*allocate)(std::size_t size, std::uint64_t attributes) = nullptr;
    void (*release)(void* address, std::uint64_t attributes) = nullptr;
    std::uint64_t descriptorHeapAttributes = 0;
    std::uint64_t committedTextureAttributes = 0;
};

struct WrittenUse {
    struct Part {
        std::uint64_t bytes = 0;
        std::uint64_t count = 0;
    };
    Part all;
    Part neverNamed;
    Part neverNamedWritable;
    Part neverNamedNoView;
    Part idle;
    Part neverWritten;
};

struct ResourceContext {
    const VulkanDevice& device;
    TitleBufferWindow& window;
    BufferArena& arena;
    const TitleAllocator* titleMemory = nullptr;
    BindlessSet* bindless = nullptr;
    TitleAddressSpace* addresses = nullptr;
};

constexpr std::uint64_t kBufferPlacementAlignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

class Resource final : public DeviceChildBase<ID3D12Resource> {
public:
    static HRESULT CreateCommitted(const ResourceContext& context, IUnknown* parent, const D3D12_HEAP_PROPERTIES& heap, D3D12_HEAP_FLAGS heapFlags,
                                   const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initial, REFIID riid, void** resource, std::string& error);
    static HRESULT CreateAtTitleAddress(const ResourceContext& context, IUnknown* parent, std::uint64_t address, const D3D12_RESOURCE_DESC& desc, REFIID riid, void** resource,
                                        std::string& error);

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
    BufferRef Buffer() const;
    std::optional<BufferRef> BufferSpan(UINT64 offset, UINT64 bytes) const;
    std::uint64_t TitleAddress() const { return titleAddress_; }
    std::uint64_t Width() const { return desc_.Width; }
    void AttachTitleMemory(void* memory, std::uint64_t attributes);
    void RememberTitleAddress(std::uint64_t address) { textureAddress_ = address; }
    std::uint64_t TextureAddress() const { return textureAddress_; }
    void SetSkippedMips(std::uint32_t skipped) { skippedMips_ = skipped; }
    std::uint32_t SkippedMips() const { return skippedMips_; }
    void RegisterGpuWritten(std::uint64_t address, bool writableAtFirst);
    void NoteListUse(bool writes);
    void NoteUse();
    static WrittenUse MeasureWrittenUse(std::uint32_t idleSeconds);
    void SetReadBack(bool on) { readBack_.store(on); }
    bool ReadBack() const { return readBack_.load(std::memory_order_relaxed); }
    static Resource* GpuWrittenSame(std::uint64_t address, const D3D12_RESOURCE_DESC& desc);
    std::uint64_t TextureBytes() const { return textureBytes_; }
    static Resource* GpuWrittenAt(std::uint64_t address, const D3D12_RESOURCE_DESC& desc);
    const TextureRef& Texture() const { return texture_; }
    UINT Maps() const { return maps_; }
    VkImageView View(const VkImageViewCreateInfo& info, std::string& error);
    struct DepthShadow {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkImageViewCreateInfo info{};
        DeviceBuffer staging;
        VkExtent3D extent{};
        VkImageView plane = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        DepthPlanePass::Source source;
        const DepthPlanePass* pass = nullptr;
        bool stencil = false;
        std::atomic<bool> planeNewer{true};
    };
    static unsigned long DepthUnorderedAccess();
    DepthShadow* EnsureDepthShadow(std::string& error);
    DepthShadow* DepthShadowOf() { return depthShadowReady_.load(std::memory_order_acquire) ? &depthShadow_ : nullptr; }
    std::size_t Views() const;

protected:
    bool Implements(REFIID riid) const override;

private:
    Resource(const ResourceContext& context, IUnknown* parent, const D3D12_HEAP_PROPERTIES& heap, D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC& desc)
        : DeviceChildBase(parent), context_(context), heap_(heap), heapFlags_(heapFlags), desc_(desc) {}
    ~Resource() override;
    bool CreateBuffer(std::string& error);
    bool CheckBufferDesc(UINT allowedFlags, std::string& error) const;
    bool CreateTexture(D3D12_RESOURCE_STATES initial, std::string& error);

    ResourceContext context_;
    D3D12_HEAP_PROPERTIES heap_;
    D3D12_HEAP_FLAGS heapFlags_;
    D3D12_RESOURCE_DESC desc_;
    std::uint64_t offset_ = 0;
    std::uint64_t titleAddress_ = 0;
    std::uint64_t gpuWrittenAddress_ = 0;
    std::uint64_t textureAddress_ = 0;
    void* titleMemory_ = nullptr;
    std::uint64_t titleMemoryAttributes_ = 0;
    bool allocated_ = false;
    TextureRef texture_;
    std::uint64_t textureBytes_ = 0;
    std::uint32_t textureBucket_ = 0;
    std::uint32_t skippedMips_ = 0;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    void* poolBlock_ = nullptr;
    VkDeviceSize poolOffset_ = 0;
    VkDeviceSize poolSize_ = 0;
    std::atomic<UINT> maps_{0};
    std::atomic<bool> readBack_{false};
    std::atomic<std::uint32_t> listUse_{0};
    std::atomic<std::uint32_t> listWrite_{0};
    std::atomic<std::uint32_t> viewsMade_{0};
    std::atomic<bool> unusedPriority_{false};
    std::uint64_t initSerial_ = 0;
    bool writableAtFirst_ = false;
    DepthShadow depthShadow_;
    std::atomic<bool> depthShadowReady_{false};
    mutable std::mutex viewLock_;
    std::vector<std::pair<VkImageViewCreateInfo, VkImageView>> views_;
};

}
