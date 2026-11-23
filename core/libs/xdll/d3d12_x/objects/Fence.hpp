#pragma once

#include "ObjectBase.hpp"
#include "VulkanDevice.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace abx::d3d12x {

class CommandQueue;

class Fence final : public DeviceChildBase<ID3D12Fence1> {
public:
    static HRESULT Create(const VulkanDevice& device, IUnknown* parent, UINT64 initial, D3D12_FENCE_FLAGS flags, REFIID riid, void** fence, std::string& error);

    UINT64 STDMETHODCALLTYPE GetCompletedValue() override;
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 value, HANDLE event) override;
    HRESULT STDMETHODCALLTYPE Signal(UINT64 value) override;
    D3D12_FENCE_FLAGS STDMETHODCALLTYPE GetCreationFlags() override { return flags_; }

    VkSemaphore Semaphore() const { return semaphore_; }
    UINT64 Promised() const;
    void Promise(UINT64 value);
    void WatchFor(CommandQueue* queue);
    std::size_t PendingEvents() const;

protected:
    bool Implements(REFIID riid) const override;

private:
    Fence(const VulkanDevice& device, IUnknown* parent, D3D12_FENCE_FLAGS flags) : DeviceChildBase(parent), device_(device), flags_(flags) {}
    ~Fence() override;
    bool StartWaiter();
    void WakeLocked();
    void RunWaiter();

    const VulkanDevice& device_;
    D3D12_FENCE_FLAGS flags_;
    VkSemaphore semaphore_ = VK_NULL_HANDLE;
    mutable std::mutex lock_;
    UINT64 promised_ = 0;
    std::vector<CommandQueue*> watchers_;
    mutable std::mutex waitLock_;
    std::multimap<UINT64, HANDLE> events_;
    std::thread waiter_;
    VkSemaphore wake_ = VK_NULL_HANDLE;
    UINT64 wakeValue_ = 0;
    bool stopping_ = false;
};

}
