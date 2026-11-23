#include "Fence.hpp"

#include "AbxApi.hpp"
#include "CommandQueue.hpp"

#include <thread>
#include <utility>

namespace abx::d3d12x {

HRESULT Fence::Create(const VulkanDevice& device, IUnknown* parent, UINT64 initial, D3D12_FENCE_FLAGS flags, REFIID riid, void** fence, std::string& error) {
    if (fence == nullptr) {
        error = "no output for the fence";
        return E_POINTER;
    }
    *fence = nullptr;
    if (flags != D3D12_FENCE_FLAG_NONE) {
        error = "fence flags " + std::to_string(static_cast<int>(flags)) + " (shared or cross-adapter fences) are not supported";
        return E_INVALIDARG;
    }
    Fence* created = new Fence(device, parent, flags);
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = initial;
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
    if (device.Vk().createSemaphore(device.Handle(), &info, nullptr, &created->semaphore_) != VK_SUCCESS) {
        created->semaphore_ = VK_NULL_HANDLE;
        created->Release();
        error = "vkCreateSemaphore failed for a timeline semaphore";
        return E_OUTOFMEMORY;
    }
    created->promised_ = initial;
    const HRESULT result = created->QueryInterface(riid, fence);
    created->Release();
    if (FAILED(result)) error = "the fence does not implement the requested interface";
    return result;
}

Fence::~Fence() {
    {
        std::lock_guard<std::mutex> guard(waitLock_);
        stopping_ = true;
        if (wake_ != VK_NULL_HANDLE) WakeLocked();
    }
    if (waiter_.joinable()) waiter_.join();
    for (const auto& [value, event] : events_) CloseHandle(event);
    if (wake_ != VK_NULL_HANDLE) device_.Vk().destroySemaphore(device_.Handle(), wake_, nullptr);
    if (semaphore_ != VK_NULL_HANDLE) device_.Vk().destroySemaphore(device_.Handle(), semaphore_, nullptr);
}

std::size_t Fence::PendingEvents() const {
    std::lock_guard<std::mutex> guard(waitLock_);
    return events_.size();
}

bool Fence::StartWaiter() {
    if (waiter_.joinable()) {
        return true;
    }
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
    if (wake_ == VK_NULL_HANDLE && device_.Vk().createSemaphore(device_.Handle(), &info, nullptr, &wake_) != VK_SUCCESS) {
        wake_ = VK_NULL_HANDLE;
        return false;
    }
    waiter_ = std::thread([this] { RunWaiter(); });
    return true;
}

void Fence::WakeLocked() {
    VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    signal.semaphore = wake_;
    signal.value = ++wakeValue_;
    device_.Vk().signalSemaphore(device_.Handle(), &signal);
}

void Fence::RunWaiter() {
    for (;;) {
        UINT64 target = 0;
        UINT64 wakeTarget = 0;
        {
            std::lock_guard<std::mutex> guard(waitLock_);
            if (stopping_) return;
            const UINT64 completed = GetCompletedValue();
            while (!events_.empty() && events_.begin()->first <= completed) {
                SetEvent(events_.begin()->second);
                CloseHandle(events_.begin()->second);
                events_.erase(events_.begin());
            }
            target = events_.empty() ? 0 : events_.begin()->first;
            wakeTarget = wakeValue_ + 1;
        }
        const VkSemaphore semaphores[2] = {wake_, semaphore_};
        const UINT64 values[2] = {wakeTarget, target};
        VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wait.flags = VK_SEMAPHORE_WAIT_ANY_BIT;
        wait.semaphoreCount = target != 0 ? 2 : 1;
        wait.pSemaphores = semaphores;
        wait.pValues = values;
        if (device_.Vk().waitSemaphores(device_.Handle(), &wait, UINT64_MAX) != VK_SUCCESS) {
            std::lock_guard<std::mutex> guard(waitLock_);
            for (const auto& [value, event] : events_) {
                SetEvent(event);
                CloseHandle(event);
            }
            events_.clear();
            return;
        }
    }
}

bool Fence::Implements(REFIID riid) const {
    return DeviceChildBase::Implements(riid) || riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12Fence) || riid == __uuidof(ID3D12Fence1);
}

UINT64 STDMETHODCALLTYPE Fence::GetCompletedValue() {
    std::uint64_t value = 0;
    if (device_.Vk().getSemaphoreCounterValue(device_.Handle(), semaphore_, &value) != VK_SUCCESS) {
        return UINT64_MAX;
    }
    return value;
}

HRESULT STDMETHODCALLTYPE Fence::SetEventOnCompletion(UINT64 value, HANDLE event) {
    if (GetCompletedValue() >= value) {
        if (event != nullptr) SetEvent(event);
        return S_OK;
    }
    VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wait.semaphoreCount = 1;
    wait.pSemaphores = &semaphore_;
    wait.pValues = &value;
    if (event == nullptr) {
        return device_.Vk().waitSemaphores(device_.Handle(), &wait, UINT64_MAX) == VK_SUCCESS ? S_OK : DXGI_ERROR_DEVICE_REMOVED;
    }
    HANDLE copy = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return E_INVALIDARG;
    }
    std::lock_guard<std::mutex> guard(waitLock_);
    if (!StartWaiter()) {
        CloseHandle(copy);
        return E_OUTOFMEMORY;
    }
    events_.emplace(value, copy);
    WakeLocked();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Fence::Signal(UINT64 value) {
    const UINT64 current = GetCompletedValue();
    if (value == current) {
        return S_OK;
    }
    if (value < current) {
        AbxUnsupported("ID3D12Fence::Signal", "setting a fence below its completed value");
    }
    VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    signal.semaphore = semaphore_;
    signal.value = value;
    if (device_.Vk().signalSemaphore(device_.Handle(), &signal) != VK_SUCCESS) {
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    Promise(value);
    return S_OK;
}

UINT64 Fence::Promised() const {
    std::lock_guard<std::mutex> guard(lock_);
    return promised_;
}

void Fence::Promise(UINT64 value) {
    std::vector<CommandQueue*> woken;
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (value <= promised_) return;
        promised_ = value;
        woken.swap(watchers_);
    }
    for (CommandQueue* queue : woken) {
        queue->Flush();
        queue->Release();
    }
}

void Fence::WatchFor(CommandQueue* queue) {
    std::lock_guard<std::mutex> guard(lock_);
    for (CommandQueue* watcher : watchers_) {
        if (watcher == queue) return;
    }
    queue->AddRef();
    watchers_.push_back(queue);
}

}
