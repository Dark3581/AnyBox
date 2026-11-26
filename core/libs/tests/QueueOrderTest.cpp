#include "BufferArena.hpp"
#include "CommandAllocator.hpp"
#include "CommandList.hpp"
#include "CommandQueue.hpp"
#include "Fence.hpp"
#include "Resource.hpp"
#include "TitleBufferWindow.hpp"
#include "VulkanDevice.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace abx::d3d12x;

namespace {

struct Parent final : IUnknown {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** object) override {
        if (object != nullptr) *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
};

int g_failed = 0;

void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

double Ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}

struct ShownLog {
    std::mutex lock;
    std::vector<int> order;
    std::vector<DWORD> threads;

    void Add(int value) {
        const std::lock_guard guard(lock);
        order.push_back(value);
        threads.push_back(GetCurrentThreadId());
    }

    std::size_t Size() {
        const std::lock_guard guard(lock);
        return order.size();
    }
};

bool Ascending(const std::vector<int>& order, int count) {
    if (order.size() != static_cast<std::size_t>(count)) return false;
    for (int index = 0; index < count; ++index) {
        if (order[static_cast<std::size_t>(index)] != index + 1) return false;
    }
    return true;
}

bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        Sleep(1);
    }
    return true;
}

void StressFenceReleases(const VulkanDevice& device, Parent& parent, CommandQueue* a, CommandQueue* b) {
    constexpr int kHeld = 512;
    std::string error;
    ID3D12Fence* left = nullptr;
    ID3D12Fence* right = nullptr;
    if (FAILED(Fence::Create(device, &parent, 0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&left), error)) ||
        FAILED(Fence::Create(device, &parent, 0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&right), error))) {
        Check(false, ("stress fences: " + error).c_str());
        return;
    }
    ShownLog shown;
    for (int index = 1; index <= kHeld; ++index) {
        a->Wait((index & 1) != 0 ? left : right, static_cast<UINT64>((index + 1) / 2));
        a->PresentOrdered([&shown, index, padding = std::string(48, 'p')] {
            if (!padding.empty()) shown.Add(index);
        });
    }
    Check(shown.Size() == 0 && a->Pending() == static_cast<std::size_t>(2 * kHeld), "512 presents behind waits on two fences are all held");
    std::mutex releasersLock;
    std::set<DWORD> releasers;
    const auto release = [&](ID3D12Fence* fence) {
        for (UINT64 value = 1; value <= kHeld / 2; ++value) {
            std::thread once([&, fence, value] {
                {
                    const std::lock_guard guard(releasersLock);
                    releasers.insert(GetCurrentThreadId());
                }
                b->Signal(fence, value);
            });
            once.join();
        }
    };
    const auto begin = std::chrono::steady_clock::now();
    std::thread first(release, left);
    std::thread second(release, right);
    first.join();
    second.join();
    const bool all = WaitFor([&] { return shown.Size() == static_cast<std::size_t>(kHeld) && a->Pending() == 0; }, std::chrono::milliseconds(5000));
    std::printf("     512 releases from two threads, each signal on a thread of its own that then exited: %.1f ms\n", Ms(begin));
    Check(all, "every held present was shown and the queue is empty");
    Check(Ascending(shown.order, kHeld), "the presents were shown in the order they were queued");
    bool onReleasers = !shown.threads.empty();
    for (const DWORD thread : shown.threads) onReleasers = onReleasers && releasers.count(thread) != 0;
    Check(onReleasers, "every present ran on the thread whose signal released its queue");
    Sleep(50);
    Check(HeapValidate(GetProcessHeap(), 0, nullptr) != FALSE, "the process heap is intact after 512 threads that ran presents exited");
    left->Release();
    right->Release();
}

void StressMemoryReleases(const VulkanDevice& device, Parent& parent, CommandQueue* a) {
    constexpr int kHeld = 128;
    std::string error;
    const std::unique_ptr<TitleBufferWindow> window = TitleBufferWindow::Create(device, 16u << 20, error);
    if (!window) {
        Check(false, ("stress window: " + error).c_str());
        return;
    }
    BufferArena arena(window->Start(), window->Size());
    const ResourceContext context{device, *window, arena};
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* done = nullptr;
    if (FAILED(CommandAllocator::Create(device, &parent, D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&allocator), error)) ||
        FAILED(CommandList::Create(context, &parent, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list), error)) ||
        FAILED(Fence::Create(device, &parent, 0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&done), error))) {
        Check(false, ("stress list: " + error).c_str());
        return;
    }
    alignas(8) static volatile std::uint64_t word = 0;
    ShownLog shown;
    bool recorded = SUCCEEDED(list->Close());
    for (int index = 1; index <= kHeld && recorded; ++index) {
        if (index > 1) recorded = SUCCEEDED(list->Reset(allocator, nullptr)) && SUCCEEDED(list->Close());
        CommandQueue::MemoryWait wait;
        wait.address = reinterpret_cast<const volatile std::uint8_t*>(&word);
        wait.titleAddress = reinterpret_cast<std::uintptr_t>(&word);
        wait.comparison = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        wait.value = static_cast<std::uint64_t>(index);
        ID3D12CommandList* const lists[] = {list};
        a->ExecuteOrdered(1, lists, {wait}, {});
        a->PresentOrdered([&shown, index, padding = std::string(48, 'p')] {
            if (!padding.empty()) shown.Add(index);
        });
    }
    Check(recorded && shown.Size() == 0 && a->Pending() == static_cast<std::size_t>(2 * kHeld), "128 presents behind lists that wait for a memory value are all held");
    std::atomic<std::uint64_t> next{0};
    const auto write = [&] {
        for (;;) {
            const std::uint64_t value = ++next;
            if (value > static_cast<std::uint64_t>(kHeld)) return;
            InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&word), static_cast<LONG64>(value));
            Sleep(1);
        }
    };
    const auto begin = std::chrono::steady_clock::now();
    std::thread first(write);
    std::thread second(write);
    first.join();
    second.join();
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&word), kHeld);
    const bool all = WaitFor([&] { return shown.Size() == static_cast<std::size_t>(kHeld) && a->Pending() == 0; }, std::chrono::milliseconds(10000));
    std::printf("     128 releases by the label writer, values written from two threads: %.1f ms\n", Ms(begin));
    Check(all, "every present released by the label writer was shown and the queue is empty");
    Check(Ascending(shown.order, kHeld), "they were shown in the order they were queued");
    const std::set<DWORD> threads(shown.threads.begin(), shown.threads.end());
    Check(threads.size() == 1 && threads.count(GetCurrentThreadId()) == 0, "all of them ran on one present thread, not on the thread that queued them");
    Sleep(50);
    Check(HeapValidate(GetProcessHeap(), 0, nullptr) != FALSE, "the process heap is intact after the label writer's releases");
    a->Signal(done, 1);
    Check(WaitFor([&] { return done->GetCompletedValue() == 1; }, std::chrono::milliseconds(5000)), "the stress lists finished on the GPU");
    list->Release();
    allocator->Release();
    done->Release();
}

}

int main() {
    DeviceRequirements requirements;
    requirements.features = {"bufferDeviceAddress", "meshShader", "shaderInt64"};
    requirements.presentation = true;
    requirements.hostMemoryImport = true;
    requirements.pageableMemory = true;
    std::string error;
    const std::unique_ptr<VulkanDevice> device = VulkanDevice::Create(requirements, error);
    if (!device) {
        std::printf("SKIP no Vulkan device with the features AnyBox needs: %s\n", error.c_str());
        return 0;
    }
    Parent parent;
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* rawA = nullptr;
    ID3D12CommandQueue* rawB = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12Fence* second = nullptr;
    if (FAILED(CommandQueue::Create(*device, &parent, desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&rawA), error)) ||
        FAILED(CommandQueue::Create(*device, &parent, desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&rawB), error)) ||
        FAILED(Fence::Create(*device, &parent, 0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence), error)) ||
        FAILED(Fence::Create(*device, &parent, 0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&second), error))) {
        std::printf("FAIL objects: %s\n", error.c_str());
        return 1;
    }
    CommandQueue* const a = static_cast<CommandQueue*>(rawA);
    CommandQueue* const b = static_cast<CommandQueue*>(rawB);
    const DWORD self = GetCurrentThreadId();

    std::vector<int> order;
    DWORD ranOn = 0;
    a->PresentOrdered([&] {
        order.push_back(1);
        ranOn = GetCurrentThreadId();
    });
    Check(order.size() == 1 && ranOn == self && a->Pending() == 0, "a present on a queue that holds nothing runs inside the call, on the calling thread");

    order.clear();
    a->Wait(fence, 1);
    Check(a->Pending() == 1, "a wait for a fence value nobody promised holds its queue");
    auto begin = std::chrono::steady_clock::now();
    a->PresentOrdered([&] {
        order.push_back(2);
        ranOn = GetCurrentThreadId();
    });
    const double enqueueMs = Ms(begin);
    Check(order.empty() && a->Pending() == 2, "a present behind the held wait returns without running");
    std::printf("     the call took %.3f ms\n", enqueueMs);
    a->Signal(second, 5);
    Check(order.empty() && a->Pending() == 3, "what follows the held present on its queue is held too");
    b->Signal(fence, 1);
    Check(order.size() == 1 && order[0] == 2 && ranOn == self, "the present runs on the thread whose signal released the queue, inside that call");
    Check(a->Pending() == 0, "the queue is empty after the present ran");
    Check(second->GetCompletedValue() == 5 || static_cast<Fence*>(second)->Promised() == 5, "the signal queued behind the present was submitted after it");

    order.clear();
    a->Wait(fence, 2);
    a->PresentOrdered([&] { order.push_back(3); });
    a->Wait(fence, 3);
    a->PresentOrdered([&] { order.push_back(4); });
    Check(order.empty() && a->Pending() == 4, "two presents behind two waits are both held");
    b->Signal(fence, 2);
    Check(order.size() == 1 && order[0] == 3 && a->Pending() == 2, "the first release shows only the first present");
    b->Signal(fence, 3);
    Check(order.size() == 2 && order[1] == 4 && a->Pending() == 0, "the second release shows the second present");

    order.clear();
    a->Wait(fence, 4);
    std::atomic<bool> inside{false};
    std::atomic<bool> finished{false};
    a->PresentOrdered([&] {
        inside.store(true);
        Sleep(150);
        order.push_back(5);
        finished.store(true);
    });
    std::thread other([&] { b->Signal(fence, 4); });
    while (!inside.load()) Sleep(1);
    begin = std::chrono::steady_clock::now();
    a->Signal(second, 6);
    const double duringMs = Ms(begin);
    Check(!finished.load() && duringMs < 50.0, "a call on the queue from another thread while its present runs returns without waiting for it");
    std::printf("     that call took %.3f ms\n", duringMs);
    other.join();
    Check(finished.load() && a->Pending() == 0 && static_cast<Fence*>(second)->Promised() == 6, "what was queued during the present is submitted when it ends");

    a->Wait(fence, 5);
    begin = std::chrono::steady_clock::now();
    const bool drained = a->Drain(std::chrono::milliseconds(300));
    const double drainMs = Ms(begin);
    Check(!drained && drainMs >= 290.0, "the old path: a present's Drain on a held queue costs its whole 300 ms limit");
    std::printf("     Drain took %.1f ms\n", drainMs);
    b->Signal(fence, 5);
    Check(a->Pending() == 0, "released at the end");

    StressFenceReleases(*device, parent, a, b);
    StressMemoryReleases(*device, parent, a);

    std::printf(g_failed == 0 ? "PASS\n" : "FAILED %d\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
