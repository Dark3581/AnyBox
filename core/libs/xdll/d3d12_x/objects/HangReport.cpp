#include "HangReport.hpp"

#include "AbxApi.hpp"
#include "ThreadStacks.hpp"
#include "TitleAddressSpace.hpp"
#include "VulkanDevice.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

namespace abx::d3d12x {

namespace {

constexpr std::uint64_t kDefaultSeconds = 20;
constexpr std::uint64_t kMaxSeconds = 3600;
constexpr int kReportsARun = 4;
constexpr std::uint64_t kSecondLookFactor = 3;
constexpr std::uint32_t kSampleMilliseconds = 1000;
constexpr std::uint32_t kLockProbeMilliseconds = 200;
constexpr std::uint32_t kCompareMilliseconds = 30000;
constexpr std::uint64_t kQueueProbeNanoseconds = 2'000'000'000ull;
constexpr std::size_t kLineChars = 1800;

std::atomic<std::uint64_t> g_lastPresent{0};
std::atomic<std::uint64_t> g_presents{0};

std::string ProbeQueue(const VulkanDevice& device) {
    std::unique_lock<std::mutex> queue(device.QueueLock(), std::try_to_lock);
    for (std::uint32_t waited = 0; !queue.owns_lock() && waited < kLockProbeMilliseconds; waited += 10) {
        Sleep(10);
        if (queue.try_lock()) break;
    }
    if (!queue.owns_lock()) {
        return "the GPU queue lock could not be taken once in " + std::to_string(kLockProbeMilliseconds) + " ms: a thread holds it (see the thread stacks)";
    }
    const DeviceFunctions& vk = device.Vk();
    VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (vk.createFence(device.Handle(), &info, nullptr, &fence) != VK_SUCCESS) return "the GPU queue lock is free; no fence could be created to ask the GPU queue";
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    const VkResult submitted = vk.queueSubmit(device.Queue(), 1, &submit, fence);
    if (submitted != VK_SUCCESS) {
        vk.destroyFence(device.Handle(), fence, nullptr);
        return "the GPU queue lock is free; the GPU queue refused an empty batch with result " + std::to_string(static_cast<int>(submitted));
    }
    queue.unlock();
    const auto begin = std::chrono::steady_clock::now();
    const VkResult waited = vk.waitForFences(device.Handle(), 1, &fence, VK_TRUE, kQueueProbeNanoseconds);
    const long long milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
    if (waited == VK_SUCCESS) {
        vk.destroyFence(device.Handle(), fence, nullptr);
        return "the GPU queue lock is free and the GPU finished an empty batch " + std::to_string(milliseconds) + " ms after it was submitted: nothing submitted before it is still running";
    }
    if (waited == VK_TIMEOUT) {
        return "the GPU queue lock is free, but the GPU did not reach an empty batch within " + std::to_string(milliseconds) +
               " ms: work submitted before it is still running or waits for a semaphore (its fence is left open)";
    }
    return "the GPU queue lock is free; waiting for an empty batch ended with result " + std::to_string(static_cast<int>(waited)) + " (-4 is a lost device)";
}

}

void HangReport::NotePresent() {
    g_lastPresent.store(GetTickCount64(), std::memory_order_relaxed);
    g_presents.fetch_add(1, std::memory_order_release);
}

std::unique_ptr<HangReport> HangReport::Start(const VulkanDevice& device, const TitleAddressSpace* space) {
    std::uint64_t seconds = kDefaultSeconds;
    if (const char* text = AbxSetting("d3d12_x.hang_report_seconds")) {
        char* end = nullptr;
        seconds = std::strtoull(text, &end, 10);
        if (end == text || *end != '\0' || seconds > kMaxSeconds) {
            AbxUnsupported("D3D12XboxCreateDevice", ("d3d12_x.hang_report_seconds must be a whole number from 0 to " + std::to_string(kMaxSeconds)).c_str());
        }
    }
    if (seconds == 0) {
        AbxLog("d3d12_x", "no hang report is written when frames stop (d3d12_x.hang_report_seconds = 0)");
        return nullptr;
    }
    AbxLog("d3d12_x",
           "when no frame is presented for %llu s after the first one (d3d12_x.hang_report_seconds, 0 turns it off) the log gets a hang report, and a second one for the "
           "same stall when it has lasted three times as long (two reports show which threads moved), at most %d of each a run: every "
           "other thread's stack as module!function+offset (functions of AnyBox's libraries by name, of other modules by their nearest export within 4 KiB, else "
           "module+offset) with the CPU time it used in the second after and its last error, threads with the same stack on one line; the title address space's "
           "operations in flight with the lock each holds or waits for; read-only copies of title pages compared with system memory (30 s at most); and whether the "
           "GPU queue still finishes an empty batch. Each thread is stopped only while its stack is read",
           static_cast<unsigned long long>(seconds), kReportsARun);
    std::unique_ptr<HangReport> report(new HangReport(device, space, seconds));
    report->thread_ = std::thread([raw = report.get()] { raw->Run(); });
    return report;
}

HangReport::~HangReport() {
    {
        const std::lock_guard guard(lock_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void HangReport::Run() {
    std::uint64_t reportedAt = 0;
    bool lookedAgain = false;
    int firstLooks = 0;
    int secondLooks = 0;
    for (;;) {
        {
            std::unique_lock guard(lock_);
            if (wake_.wait_for(guard, std::chrono::seconds(1), [&] { return stop_; })) return;
        }
        const std::uint64_t presents = g_presents.load(std::memory_order_acquire);
        if (presents == 0) continue;
        const std::uint64_t idle = GetTickCount64() - g_lastPresent.load(std::memory_order_relaxed);
        if (reportedAt != 0 && presents != reportedAt) {
            AbxLog("d3d12_x", "frames are presented again after the stall the last hang report was written for (%llu present(s) since)",
                   static_cast<unsigned long long>(presents - reportedAt));
            reportedAt = 0;
            lookedAgain = false;
        }
        if (idle < seconds_ * 1000) continue;
        const bool late = idle >= kSecondLookFactor * seconds_ * 1000;
        if (reportedAt == 0) {
            if (firstLooks < kReportsARun) {
                reportedAt = presents;
                ++firstLooks;
                Report(idle, presents, false);
                continue;
            }
            if (!late) continue;
            reportedAt = presents;
        }
        if (!late || lookedAgain || secondLooks >= kReportsARun) continue;
        lookedAgain = true;
        ++secondLooks;
        Report(idle, presents, true);
    }
}

void HangReport::Report(std::uint64_t idleMilliseconds, std::uint64_t presents, bool secondLook) {
    AbxLog("d3d12_x", "hang report begins (%s): no frame was presented for %llu ms (the last present was number %llu) while the process lives",
           secondLook ? "second look at the same stall: a thread whose stack and CPU time are as in the first look did not move" : "first look",
           static_cast<unsigned long long>(idleMilliseconds), static_cast<unsigned long long>(presents));
    for (const std::string& line : DescribeThreadStacks(kSampleMilliseconds, kLineChars)) AbxLog("d3d12_x", "hang report: %s", line.c_str());
    if (space_ != nullptr) {
        for (const std::string& line : space_->DescribeActivity(kLockProbeMilliseconds, kCompareMilliseconds)) AbxLog("d3d12_x", "hang report: %s", line.c_str());
        std::uint64_t address = 0;
        std::uint64_t code = 0;
        std::uint32_t thread = 0;
        if (TitleAddressSpace::RepeatedAccesses(address, code, thread) > 1) {
            AbxLog("d3d12_x", "hang report: the code of the longest row of repeated accesses to a guarded page is %s", DescribeCode(code).c_str());
        }
    }
    AbxLog("d3d12_x", "hang report: %s", ProbeQueue(device_).c_str());
    AbxLog("d3d12_x", "hang report ends");
}

}
