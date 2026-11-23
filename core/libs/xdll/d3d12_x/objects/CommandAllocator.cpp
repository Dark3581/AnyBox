#include "CommandAllocator.hpp"

namespace abx::d3d12x {

HRESULT CommandAllocator::Create(const VulkanDevice& device, IUnknown* parent, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** allocator, std::string& error) {
    if (allocator == nullptr) {
        error = "no output for the command allocator";
        return E_POINTER;
    }
    *allocator = nullptr;
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE && type != D3D12_COMMAND_LIST_TYPE_COPY) {
        error = "command list type " + std::to_string(static_cast<int>(type)) + " is not direct, compute or copy";
        return E_INVALIDARG;
    }
    CommandAllocator* created = new CommandAllocator(device, parent, type);
    VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    info.queueFamilyIndex = device.QueueFamily();
    if (device.Vk().createCommandPool(device.Handle(), &info, nullptr, &created->pool_) != VK_SUCCESS) {
        created->pool_ = VK_NULL_HANDLE;
        created->Release();
        error = "vkCreateCommandPool failed";
        return E_OUTOFMEMORY;
    }
    const HRESULT result = created->QueryInterface(riid, allocator);
    created->Release();
    if (FAILED(result)) error = "the command allocator does not implement the requested interface";
    return result;
}

CommandAllocator::~CommandAllocator() {
    if (pool_ != VK_NULL_HANDLE) device_.Vk().destroyCommandPool(device_.Handle(), pool_, nullptr);
}

bool CommandAllocator::Implements(REFIID riid) const {
    return DeviceChildBase::Implements(riid) || riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12CommandAllocator);
}

HRESULT STDMETHODCALLTYPE CommandAllocator::Reset() {
    if (device_.Vk().resetCommandPool(device_.Handle(), pool_, 0) != VK_SUCCESS) {
        return E_FAIL;
    }
    next_ = 0;
    return S_OK;
}

VkCommandBuffer CommandAllocator::Next(std::string& error) {
    if (next_ < buffers_.size()) {
        return buffers_[next_++];
    }
    VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    info.commandPool = pool_;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    VkCommandBuffer buffer = VK_NULL_HANDLE;
    if (device_.Vk().allocateCommandBuffers(device_.Handle(), &info, &buffer) != VK_SUCCESS) {
        error = "vkAllocateCommandBuffers failed";
        return VK_NULL_HANDLE;
    }
    buffers_.push_back(buffer);
    next_ = buffers_.size();
    return buffer;
}

}
