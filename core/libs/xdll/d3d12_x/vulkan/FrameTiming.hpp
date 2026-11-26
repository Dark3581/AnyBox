#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace abx::d3d12x {

enum class TimedPart : std::uint32_t {
    Present,
    PresentAcquire,
    PresentSync,
    Execute,
    QueueSubmit,
    MemoryWaitHeld,
    ListWork,
    PipelineWait,
    ListBarriers,
    ListCopies,
    ListClose,
    TextureMake,
    TextureRefresh,
    TextureFlush,
    Readback,
    GpuPages,
    AddressIdleWait,
    SyncSubmit,
    DesktopRest,
    PresentHeld,
    PresentLater,
    ReadbackWrite,
    RefreshLock,
    RefreshSubmit,
    RefreshRelease,
    QueueLockWait,
    Count
};

constexpr std::size_t kTimedParts = static_cast<std::size_t>(TimedPart::Count);

struct FrameTimingTotals {
    std::atomic<std::uint64_t> nanoseconds[kTimedParts] = {};
    std::atomic<std::uint64_t> calls[kTimedParts] = {};
};

inline FrameTimingTotals g_frameTiming;

inline void AddTime(TimedPart part, std::chrono::steady_clock::duration spent) {
    const std::size_t index = static_cast<std::size_t>(part);
    g_frameTiming.nanoseconds[index].fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(spent).count()), std::memory_order_relaxed);
    g_frameTiming.calls[index].fetch_add(1, std::memory_order_relaxed);
}

class TimedScope {
public:
    explicit TimedScope(TimedPart part) : part_(part), begin_(std::chrono::steady_clock::now()) {}
    ~TimedScope() { AddTime(part_, std::chrono::steady_clock::now() - begin_); }
    TimedScope(const TimedScope&) = delete;
    TimedScope& operator=(const TimedScope&) = delete;

private:
    TimedPart part_;
    std::chrono::steady_clock::time_point begin_;
};

}
