#pragma once

#include "ObjectBase.hpp"
#include "VulkanDevice.hpp"

#include <cstddef>
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
};

}
