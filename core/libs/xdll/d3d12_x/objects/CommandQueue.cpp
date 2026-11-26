#include "CommandQueue.hpp"

#include "AbxApi.hpp"
#include "CommandList.hpp"
#include "Fence.hpp"
#include "FrameTiming.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace abx::d3d12x {

namespace {

std::string LoggedFaultReport(const VulkanDevice& device) {
    constexpr std::size_t kPiece = 1800;
    const std::string report = device.FaultReport();
    const std::size_t pieces = (report.size() + kPiece - 1) / kPiece;
    for (std::size_t piece = 0; piece < pieces; ++piece) {
        AbxLog("d3d12_x", "the GPU device was lost; its whole fault report, part %zu of %zu: %s", piece + 1, pieces, report.substr(piece * kPiece, kPiece).c_str());
    }
    return report.substr(0, 700);
}

std::chrono::milliseconds MemoryWaitLimit() {
    static const std::chrono::milliseconds limit = [] {
        const char* value = AbxSetting("d3d12_x.memory_wait_ms");
        const unsigned long asked = value != nullptr ? std::strtoul(value, nullptr, 10) : 0;
        return std::chrono::milliseconds(asked >= 1 && asked <= 60000 ? asked : 200);
    }();
    return limit;
}

struct LabelBatch {
    std::uint64_t point = 0;
    std::vector<CommandQueue::MemoryLabel> labels;
};

std::recursive_mutex g_flushAll;
std::mutex g_ordering;
std::condition_variable g_wake;
std::map<std::uint64_t, std::uint64_t> g_promised;
std::vector<CommandQueue*> g_queues;
std::deque<LabelBatch> g_batches;
VkSemaphore g_timeline = VK_NULL_HANDLE;
std::uint64_t g_point = 0;
std::atomic<int> g_blocked{0};
bool g_writerStarted = false;

constexpr DWORD kPresentThread = 0;

struct DuePresent {
    DWORD owner = kPresentThread;
    CommandQueue* queue = nullptr;
    std::function<void()> present;
};

struct DuePresents {
    std::mutex lock;
    std::condition_variable wake;
    std::deque<DuePresent> due;
    bool threadStarted = false;
};

DuePresents& Due() {
    static DuePresents* const due = new DuePresents;
    return *due;
}

thread_local bool t_runningPresents = false;
thread_local bool t_presentThread = false;
std::atomic<std::uint64_t> g_presentsLater{0};
std::atomic<std::uint64_t> g_waitsGivenUp{0};

DWORD PresentOwner() {
    return t_presentThread ? kPresentThread : GetCurrentThreadId();
}

void AddDuePresent(CommandQueue* queue, std::function<void()> present) {
    DuePresents& due = Due();
    const std::lock_guard guard(due.lock);
    due.due.push_back({PresentOwner(), queue, std::move(present)});
}

bool TakeDuePresent(DWORD owner, DuePresent& taken) {
    DuePresents& due = Due();
    const std::lock_guard guard(due.lock);
    const auto found = std::find_if(due.due.begin(), due.due.end(), [owner](const DuePresent& present) { return present.owner == owner; });
    if (found == due.due.end()) return false;
    taken = std::move(*found);
    due.due.erase(found);
    return true;
}

bool PresentThreadHasWork(const DuePresents& due) {
    return std::any_of(due.due.begin(), due.due.end(), [](const DuePresent& present) { return present.owner == kPresentThread; });
}

void RunPresentThread() {
    t_presentThread = true;
    DuePresents& due = Due();
    for (;;) {
        {
            std::unique_lock guard(due.lock);
            due.wake.wait(guard, [&due] { return PresentThreadHasWork(due); });
        }
        CommandQueue::RunDuePresents();
    }
}

void HandDuePresentsOn() {
    const DWORD self = GetCurrentThreadId();
    DuePresents& due = Due();
    bool handed = false;
    bool started = false;
    {
        const std::lock_guard guard(due.lock);
        for (DuePresent& present : due.due) {
            if (present.owner != self) continue;
            present.owner = kPresentThread;
            handed = true;
        }
        if (!handed) return;
        if (!due.threadStarted) {
            due.threadStarted = true;
            started = true;
            std::thread(RunPresentThread).detach();
        }
    }
    due.wake.notify_all();
    if (started) {
        AbxLog("d3d12_x", "presents that the label writer released are shown on one present thread that lives as long as the process, in their queue's order "
                          "(d3d12_x.present_in_queue_order)");
    }
}

bool Holds(std::uint32_t comparison, std::uint64_t current, std::uint64_t value) {
    switch (comparison) {
    case D3D12_COMPARISON_FUNC_NEVER: return false;
    case D3D12_COMPARISON_FUNC_LESS: return current < value;
    case D3D12_COMPARISON_FUNC_EQUAL: return current == value;
    case D3D12_COMPARISON_FUNC_LESS_EQUAL: return current <= value;
    case D3D12_COMPARISON_FUNC_GREATER: return current > value;
    case D3D12_COMPARISON_FUNC_NOT_EQUAL: return current != value;
    case D3D12_COMPARISON_FUNC_GREATER_EQUAL: return current >= value;
    default: return true;
    }
}

std::uint64_t Current(const CommandQueue::MemoryWait& wait) {
    return wait.narrow ? *reinterpret_cast<const volatile std::uint32_t*>(wait.address) : *reinterpret_cast<const volatile std::uint64_t*>(wait.address);
}

const CommandQueue::MemoryWait* FirstUnmet(const std::vector<CommandQueue::MemoryWait>& waits) {
    const std::lock_guard guard(g_ordering);
    for (const CommandQueue::MemoryWait& wait : waits) {
        if (Holds(wait.comparison, Current(wait) & wait.mask, wait.value & wait.mask)) continue;
        const auto promised = g_promised.find(wait.titleAddress);
        if (promised != g_promised.end() && Holds(wait.comparison, promised->second & wait.mask, wait.value & wait.mask)) continue;
        return &wait;
    }
    return nullptr;
}

void FlushQueues(CommandQueue* except) {
    const std::lock_guard all(g_flushAll);
    std::vector<CommandQueue*> queues;
    {
        const std::lock_guard guard(g_ordering);
        queues = g_queues;
    }
    for (CommandQueue* queue : queues) {
        if (queue != except) queue->Flush();
    }
}

void RunLabelWriter(const VulkanDevice* device) {
    for (;;) {
        std::uint64_t target = 0;
        VkSemaphore timeline = VK_NULL_HANDLE;
        {
            std::unique_lock guard(g_ordering);
            if (g_batches.empty()) g_wake.wait_for(guard, std::chrono::milliseconds(g_blocked.load() > 0 ? 2 : 200));
            if (!g_batches.empty()) target = g_batches.front().point;
            timeline = g_timeline;
        }
        if (target != 0) {
            VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wait.semaphoreCount = 1;
            wait.pSemaphores = &timeline;
            wait.pValues = &target;
            if (device->Vk().waitSemaphores(device->Handle(), &wait, 5'000'000) == VK_ERROR_DEVICE_LOST) {
                const std::string detail = "the GPU device was lost while lists with labels were running" + device->CheckpointReport() + "; " + LoggedFaultReport(*device);
                AbxUnsupported("D3D12 command queue (label writer)", detail.c_str());
            }
        }
        std::vector<LabelBatch> ready;
        if (target != 0) {
            std::uint64_t reached = 0;
            if (device->Vk().getSemaphoreCounterValue(device->Handle(), timeline, &reached) == VK_SUCCESS) {
                const std::lock_guard guard(g_ordering);
                while (!g_batches.empty() && g_batches.front().point <= reached) {
                    ready.push_back(std::move(g_batches.front()));
                    g_batches.pop_front();
                }
            }
        }
        for (const LabelBatch& batch : ready) {
            for (const CommandQueue::MemoryLabel& label : batch.labels) {
                SIZE_T written = 0;
                if (WriteProcessMemory(GetCurrentProcess(), const_cast<std::uint64_t*>(label.address), &label.value, sizeof(label.value), &written) && written == sizeof(label.value)) continue;
                static std::atomic<int> reported{0};
                if (reported.fetch_add(1) < 4) {
                    AbxLog("d3d12_x", "a label for 0x%llx could not be written when its list finished: the title released that memory (error %lu)",
                           static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(label.address)), GetLastError());
                }
            }
        }
        if (!ready.empty() || g_blocked.load() > 0) FlushQueues(nullptr);
        HandDuePresentsOn();
    }
}

void StartWriterLocked(const VulkanDevice& device) {
    if (g_writerStarted) return;
    g_writerStarted = true;
    std::thread(RunLabelWriter, &device).detach();
    AbxLog("d3d12_x", "memory labels and waits order work across the title's command queues: a list that waits for a value is held, with what follows on its queue, until a submitted "
                      "list's label promises it (at most %lld ms, d3d12_x.memory_wait_ms); label values reach title memory when the GPU has finished that list",
           static_cast<long long>(MemoryWaitLimit().count()));
}

bool SubmitLabels(const VulkanDevice& device, const std::vector<CommandQueue::MemoryLabel>& labels) {
    const std::lock_guard guard(g_ordering);
    if (g_timeline == VK_NULL_HANDLE) {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        if (device.Vk().createSemaphore(device.Handle(), &info, nullptr, &g_timeline) != VK_SUCCESS) {
            g_timeline = VK_NULL_HANDLE;
            return false;
        }
    }
    const std::uint64_t point = ++g_point;
    VkSemaphoreSubmitInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    semaphore.semaphore = g_timeline;
    semaphore.value = point;
    semaphore.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &semaphore;
    if (device.Vk().queueSubmit2(device.Queue(), 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) return false;
    g_batches.push_back({point, labels});
    for (const CommandQueue::MemoryLabel& label : labels) g_promised[reinterpret_cast<std::uintptr_t>(label.address)] = label.value;
    StartWriterLocked(device);
    g_wake.notify_all();
    return true;
}

void ReportTimeout(const CommandQueue::MemoryWait& wait) {
    const std::uint64_t count = ++g_waitsGivenUp;
    if (count > 8 && (count & (count - 1)) != 0) return;
    std::uint64_t promised = 0;
    bool known = false;
    {
        const std::lock_guard guard(g_ordering);
        const auto found = g_promised.find(wait.titleAddress);
        known = found != g_promised.end();
        if (known) promised = found->second;
    }
    AbxLog("d3d12_x", "a memory wait was given up after %lld ms and its list runs anyway: 0x%llx holds %llu (%s%llu), comparison %u with %llu (%llu so far)",
           static_cast<long long>(MemoryWaitLimit().count()), static_cast<unsigned long long>(wait.titleAddress), static_cast<unsigned long long>(Current(wait)),
           known ? "last label submitted for it: " : "no label was ever submitted for it: ", static_cast<unsigned long long>(promised), wait.comparison,
           static_cast<unsigned long long>(wait.value), static_cast<unsigned long long>(count));
}

}

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
    {
        const std::lock_guard guard(g_ordering);
        g_queues.push_back(created);
    }
    const HRESULT result = created->QueryInterface(riid, queue);
    created->Release();
    if (FAILED(result)) error = "the command queue does not implement the requested interface";
    return result;
}

CommandQueue::~CommandQueue() {
    {
        const std::lock_guard all(g_flushAll);
        const std::lock_guard guard(g_ordering);
        g_queues.erase(std::remove(g_queues.begin(), g_queues.end(), this), g_queues.end());
        if (memoryBlocked_) --g_blocked;
    }
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
    ExecuteOrdered(count, lists, {}, {});
}

void CommandQueue::PresentOrdered(std::function<void()> present) {
    Operation operation;
    operation.present = std::move(present);
    Enqueue(std::move(operation));
}

std::uint64_t CommandQueue::MemoryWaitsGivenUp() {
    return g_waitsGivenUp.load(std::memory_order_relaxed);
}

void CommandQueue::EndPresent() {
    {
        const std::lock_guard<std::mutex> guard(lock_);
        presenting_ = false;
    }
    Flush();
}

void CommandQueue::RunDuePresents() {
    if (t_runningPresents) return;
    t_runningPresents = true;
    const DWORD owner = PresentOwner();
    for (;;) {
        DuePresent due;
        if (!TakeDuePresent(owner, due)) break;
        {
            const TimedScope timed(TimedPart::PresentLater);
            due.present();
        }
        const std::uint64_t count = ++g_presentsLater;
        if (count == 1 || (count >= 64 && (count & (count - 1)) == 0)) {
            AbxLog("d3d12_x", "a present whose queue still held lists waiting for a memory value or a fence was shown after those lists, in the queue's order, instead of "
                              "waiting for them at the call (%llu so far; d3d12_x.present_in_queue_order)",
                   static_cast<unsigned long long>(count));
        }
        due.queue->EndPresent();
        due.queue->Release();
    }
    t_runningPresents = false;
}

bool CommandQueue::Drain(std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    for (;;) {
        Flush();
        RunDuePresents();
        if (Pending() == 0) return true;
        if (std::chrono::steady_clock::now() > deadline) return false;
        Sleep(1);
    }
}

void CommandQueue::ExecuteOrdered(UINT count, ID3D12CommandList* const* lists, std::vector<MemoryWait> waits, std::vector<MemoryLabel> labels) {
    const char* function = "ID3D12CommandQueue::ExecuteCommandLists";
    if (count == 0) return;
    const TimedScope timed(TimedPart::Execute);
    if (lists == nullptr) AbxUnsupported(function, "command lists without a list array");
    Operation operation;
    operation.waits = std::move(waits);
    operation.labels = std::move(labels);
    operation.commands.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        CommandList* list = dynamic_cast<CommandList*>(lists[index]);
        if (list == nullptr) AbxUnsupported(function, "a command list that AnyBox did not create");
        if (!list->Closed()) AbxUnsupported(function, "a command list that is still recording");
        if (list->GetType() != desc_.Type) AbxUnsupported(function, "a command list whose type differs from the queue's");
        const std::vector<VkCommandBuffer> buffers = list->CommandBuffers();
        operation.commands.insert(operation.commands.end(), buffers.begin(), buffers.end());
        operation.listBuffers.push_back(static_cast<std::uint32_t>(buffers.size()));
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
    RunDuePresents();
    return S_OK;
}

std::size_t CommandQueue::Pending() const {
    std::lock_guard<std::mutex> guard(lock_);
    return pending_.size() + (presenting_ ? 1 : 0);
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
        static const bool perList = [] {
            const char* value = AbxSetting("d3d12_x.submit_per_list");
            const bool on = value == nullptr || std::strtoul(value, nullptr, 10) != 0;
            AbxLog("d3d12_x", "command lists of one ExecuteCommandLists call are submitted %s (d3d12_x.submit_per_list), so the system's GPU watchdog times %s",
                   on ? "one by one" : "together", on ? "each list on its own" : "the whole call");
            return on;
        }();
        const TimedScope timed(TimedPart::QueueSubmit);
        const auto lockBegin = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> guard(device_.QueueLock());
        AddTime(TimedPart::QueueLockWait, std::chrono::steady_clock::now() - lockBegin);
        if (perList) {
            std::size_t first = 0;
            for (std::size_t list = 0; first < buffers.size(); ++list) {
                const std::size_t inList = list < operation.listBuffers.size() ? (std::min)(static_cast<std::size_t>(operation.listBuffers[list]), buffers.size() - first) : 1;
                VkSubmitInfo2 one{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
                one.commandBufferInfoCount = static_cast<std::uint32_t>((std::max)(inList, std::size_t{1}));
                one.pCommandBufferInfos = buffers.data() + first;
                if (device_.Vk().queueSubmit2(device_.Queue(), 1, &one, VK_NULL_HANDLE) != VK_SUCCESS) return false;
                first += one.commandBufferInfoCount;
            }
        } else {
            submit.commandBufferInfoCount = static_cast<std::uint32_t>(buffers.size());
            submit.pCommandBufferInfos = buffers.data();
            if (device_.Vk().queueSubmit2(device_.Queue(), 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) return false;
        }
        return operation.labels.empty() || SubmitLabels(device_, operation.labels);
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
    bool labelled = false;
    {
        std::unique_lock<std::mutex> guard(lock_);
        if (flushing_) {
            again_ = true;
            return;
        }
        flushing_ = true;
        bool blocked = false;
        do {
            again_ = false;
            blocked = false;
            while (!pending_.empty() && !presenting_) {
                Operation& front = pending_.front();
                if (front.present) {
                    presenting_ = true;
                    AddRef();
                    AddDuePresent(this, std::move(front.present));
                    pending_.pop_front();
                    break;
                }
                if (front.wait && front.fence->Promised() < front.value && front.fence->GetCompletedValue() < front.value) {
                    front.fence->WatchFor(this);
                    if (front.fence->Promised() < front.value) break;
                    continue;
                }
                if (!front.waits.empty()) {
                    if (const MemoryWait* unmet = FirstUnmet(front.waits)) {
                        const auto now = std::chrono::steady_clock::now();
                        if (front.blockedSince == std::chrono::steady_clock::time_point{}) front.blockedSince = now;
                        if (now - front.blockedSince < MemoryWaitLimit()) {
                            blocked = true;
                            break;
                        }
                        ReportTimeout(*unmet);
                    }
                }
                if (front.blockedSince != std::chrono::steady_clock::time_point{}) AddTime(TimedPart::MemoryWaitHeld, std::chrono::steady_clock::now() - front.blockedSince);
                const Operation operation = std::move(pending_.front());
                pending_.pop_front();
                if (!Submit(operation)) {
                    const std::string detail = "vkQueueSubmit2 failed (a lost device fails every later submit)" + device_.CheckpointReport() + "; " + LoggedFaultReport(device_);
                    AbxUnsupported(operation.fence == nullptr ? "ID3D12CommandQueue::ExecuteCommandLists"
                                   : operation.wait          ? "ID3D12CommandQueue::Wait"
                                                             : "ID3D12CommandQueue::Signal",
                                   detail.c_str());
                }
                if (!operation.labels.empty()) labelled = true;
                if (operation.fence == nullptr) continue;
                if (!operation.wait) promised.emplace_back(operation.fence, operation.value);
                else operation.fence->Release();
            }
        } while (again_);
        flushing_ = false;
        if (blocked != memoryBlocked_) {
            memoryBlocked_ = blocked;
            g_blocked += blocked ? 1 : -1;
            if (blocked) {
                const std::lock_guard ordering(g_ordering);
                StartWriterLocked(device_);
                g_wake.notify_all();
            }
        }
    }
    for (const auto& [fence, value] : promised) {
        fence->Promise(value);
        fence->Release();
    }
    if (labelled) FlushQueues(this);
}

}
