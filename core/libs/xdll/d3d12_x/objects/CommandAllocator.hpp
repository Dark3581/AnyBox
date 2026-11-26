#pragma once

#include "ObjectBase.hpp"
#include "VulkanDevice.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace abx::d3d12x {

class CommandAllocator final : public DeviceChildBase<ID3D12CommandAllocator> {
public:
    static HRESULT Create(const VulkanDevice& device, IUnknown* parent, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** allocator, std::string& error);

    HRESULT STDMETHODCALLTYPE Reset() override;

    D3D12_COMMAND_LIST_TYPE Type() const { return type_; }
    VkCommandBuffer Next(std::string& error);
    std::size_t Allocated() const { return buffers_.size(); }
    bool Scratch(VkDeviceSize bytes, VkBuffer& buffer, std::string& error);
    struct TransientBlock {
        std::uintptr_t base = 0;
        std::size_t size = 0;
        std::size_t used = 0;
    };
    std::uintptr_t Transient(std::size_t bytes, std::size_t alignment, TransientBlock*& created, std::string& error);
    static std::uintptr_t ReserveHigh(std::size_t size, std::string& error);

protected:
    bool Implements(REFIID riid) const override;

private:
    CommandAllocator(const VulkanDevice& device, IUnknown* parent, D3D12_COMMAND_LIST_TYPE type) : DeviceChildBase(parent), device_(device), type_(type) {}
    ~CommandAllocator() override;

    const VulkanDevice& device_;
    D3D12_COMMAND_LIST_TYPE type_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> buffers_;
    std::size_t next_ = 0;
    struct ScratchBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
    };
    std::vector<ScratchBuffer> scratch_;
    void ReturnScratch();
    static std::vector<ScratchBuffer>& FreeScratch();
    std::vector<TransientBlock> transient_;
    std::size_t transientNext_ = 0;
};

}
