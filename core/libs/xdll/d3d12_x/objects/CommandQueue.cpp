#include "CommandQueue.hpp"

#include "AbxApi.hpp"
#include "CommandList.hpp"
#include "Fence.hpp"

#include <cmath>
#include <utility>
#include <vector>

namespace abx::d3d12x {

HRESULT CommandQueue::Create(const VulkanDevice& device, IUnknown* parent, const D3D12_COMMAND_QUEUE_DESC& desc, REFIID riid, void** queue, std::string& error) {
    if (queue == nullptr) {
        error = "no output for the command queue";
        return E_POINTER;
    }
    *queue = nullptr;
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT && desc.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE && desc.Type != D3D12_COMMAND_LIST_TYPE_COPY) {
        error = "command queue type " + std::to_string(static_cast<int>(desc.Type)) + " is not a direct, compute or copy queue";
        return E_INVALIDARG;
    }
    if (desc.NodeMask > 1) {
        error = "node mask " + std::to_string(desc.NodeMask) + " names a GPU node other than the first";
        return E_INVALIDARG;
    }
    if ((static_cast<UINT>(desc.Flags) & ~static_cast<UINT>(D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT)) != 0) {
        error = "command queue flags " + std::to_string(static_cast<int>(desc.Flags)) + " are unknown";
        return E_INVALIDARG;
    }
    CommandQueue* created = new CommandQueue(device, parent, desc);
    const HRESULT result = created->QueryInterface(riid, queue);
    created->Release();
    if (FAILED(result)) error = "the command queue does not implement the requested interface";
    return result;
}

CommandQueue::~CommandQueue() {
    for (const Operation& operation : pending_) {
        if (operation.fence != nullptr) operation.fence->Release();
    }
}

bool CommandQueue::Implements(REFIID riid) const {
    return DeviceChildBase::Implements(riid) || riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12CommandQueue);
}

void STDMETHODCALLTYPE CommandQueue::UpdateTileMappings(ID3D12Resource*, UINT, const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*, ID3D12Heap*, UINT,
                                                        const D3D12_TILE_RANGE_FLAGS*, const UINT*, const UINT*, D3D12_TILE_MAPPING_FLAGS) {
    AbxUnsupported("ID3D12CommandQueue::UpdateTileMappings", "reserved resource tile mappings");
}

void STDMETHODCALLTYPE CommandQueue::CopyTileMappings(ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*, ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*,
                                                      const D3D12_TILE_REGION_SIZE*, D3D12_TILE_MAPPING_FLAGS) {
    AbxUnsupported("ID3D12CommandQueue::CopyTileMappings", "reserved resource tile mappings");
}

void STDMETHODCALLTYPE CommandQueue::ExecuteCommandLists(UINT count, ID3D12CommandList* const* lists) {
    const char* function = "ID3D12CommandQueue::ExecuteCommandLists";
    if (count == 0) return;
    if (lists == nullptr) AbxUnsupported(function, "command lists without a list array");
    Operation operation;
    operation.commands.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        CommandList* list = dynamic_cast<CommandList*>(lists[index]);
        if (list == nullptr) AbxUnsupported(function, "a command list that AnyBox did not create");
        if (!list->Closed()) AbxUnsupported(function, "a command list that is still recording");
        if (list->GetType() != desc_.Type) AbxUnsupported(function, "a command list whose type differs from the queue's");
        operation.commands.push_back(list->Commands());
    }
    Enqueue(std::move(operation));
}

HRESULT STDMETHODCALLTYPE CommandQueue::Signal(ID3D12Fence* fence, UINT64 value) {
    Fence* ours = dynamic_cast<Fence*>(fence);
    if (ours == nullptr) return E_INVALIDARG;
    ours->AddRef();
    return Enqueue({false, ours, value});
}

HRESULT STDMETHODCALLTYPE CommandQueue::Wait(ID3D12Fence* fence, UINT64 value) {
    Fence* ours = dynamic_cast<Fence*>(fence);
    if (ours == nullptr) return E_INVALIDARG;
    ours->AddRef();
    return Enqueue({true, ours, value});
}

HRESULT STDMETHODCALLTYPE CommandQueue::GetTimestampFrequency(UINT64* frequency) {
    if (frequency == nullptr) return E_POINTER;
    const float period = device_.Properties().limits.timestampPeriod;
    if (!(period > 0.0f)) return E_FAIL;
    *frequency = static_cast<UINT64>(std::llround(1.0e9 / period));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CommandQueue::GetClockCalibration(UINT64*, UINT64*) {
    AbxUnsupported("ID3D12CommandQueue::GetClockCalibration", "GPU and CPU clock calibration");
}

HRESULT CommandQueue::Enqueue(Operation operation) {
    {
        std::lock_guard<std::mutex> guard(lock_);
        pending_.push_back(std::move(operation));
    }
    Flush();
    return S_OK;
}

std::size_t CommandQueue::Pending() const {
    std::lock_guard<std::mutex> guard(lock_);
    return pending_.size();
}

bool CommandQueue::Submit(const Operation& operation) {
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    std::vector<VkCommandBufferSubmitInfo> buffers;
    if (operation.fence == nullptr) {
        buffers.reserve(operation.commands.size());
        for (VkCommandBuffer commands : operation.commands) {
            VkCommandBufferSubmitInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
            info.commandBuffer = commands;
            buffers.push_back(info);
        }
        submit.commandBufferInfoCount = static_cast<std::uint32_t>(buffers.size());
        submit.pCommandBufferInfos = buffers.data();
        std::lock_guard<std::mutex> guard(device_.QueueLock());
        return device_.Vk().queueSubmit2(device_.Queue(), 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
    }
    VkSemaphoreSubmitInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    semaphore.semaphore = operation.fence->Semaphore();
    semaphore.value = operation.value;
    semaphore.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    if (operation.wait) {
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &semaphore;
    } else {
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &semaphore;
    }
    std::lock_guard<std::mutex> guard(device_.QueueLock());
    return device_.Vk().queueSubmit2(device_.Queue(), 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
}

void CommandQueue::Flush() {
    std::vector<std::pair<Fence*, UINT64>> promised;
    {
        std::unique_lock<std::mutex> guard(lock_);
        if (flushing_) {
            again_ = true;
            return;
        }
        flushing_ = true;
        do {
            again_ = false;
            while (!pending_.empty()) {
                const Operation& front = pending_.front();
                if (front.wait && front.fence->Promised() < front.value && front.fence->GetCompletedValue() < front.value) {
                    front.fence->WatchFor(this);
                    if (front.fence->Promised() < front.value) break;
                    continue;
                }
                const Operation operation = std::move(pending_.front());
                pending_.pop_front();
                if (!Submit(operation)) {
                    AbxUnsupported(operation.fence == nullptr ? "ID3D12CommandQueue::ExecuteCommandLists"
                                   : operation.wait          ? "ID3D12CommandQueue::Wait"
                                                             : "ID3D12CommandQueue::Signal",
                                   "vkQueueSubmit2 failed");
                }
                if (operation.fence == nullptr) continue;
                if (!operation.wait) promised.emplace_back(operation.fence, operation.value);
                else operation.fence->Release();
            }
        } while (again_);
        flushing_ = false;
    }
    for (const auto& [fence, value] : promised) {
        fence->Promise(value);
        fence->Release();
    }
}

}
