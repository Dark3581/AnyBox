#include "Fence.hpp"

#include "AbxApi.hpp"
#include "CommandQueue.hpp"

#include <thread>

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
    if (semaphore_ != VK_NULL_HANDLE) device_.Vk().destroySemaphore(device_.Handle(), semaphore_, nullptr);
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
    AddRef();
    std::thread([this, value, copy] {
        VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        info.semaphoreCount = 1;
        info.pSemaphores = &semaphore_;
        info.pValues = &value;
        if (device_.Vk().waitSemaphores(device_.Handle(), &info, UINT64_MAX) == VK_SUCCESS) SetEvent(copy);
        CloseHandle(copy);
        Release();
    }).detach();
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
