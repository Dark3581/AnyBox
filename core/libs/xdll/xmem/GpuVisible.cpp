#include "GpuVisible.hpp"

#include "xdll/xmem/xmem.hpp"

#include <windows.h>

#include <map>
#include <mutex>
#include <vector>

namespace abx::xmem {

namespace {

constexpr std::uint32_t kGraphicsAccess = PAGE_GRAPHICS_READONLY | PAGE_GRAPHICS_READWRITE | PAGE_GRAPHICS_EXECUTE | PAGE_GRAPHICS_EXECUTE_READ | PAGE_GRAPHICS_EXECUTE_READWRITE;

struct Watch {
    std::recursive_mutex lock;
    std::map<std::uint64_t, std::uint64_t> available;
    AbxGpuMemoryListener listener = nullptr;
    void* context = nullptr;
};

Watch& State() {
    static Watch* watch = new Watch();
    return *watch;
}

void Remove(Watch& watch, std::uint64_t start, std::uint64_t end) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> kept;
    for (auto it = watch.available.begin(); it != watch.available.end();) {
        const std::uint64_t rangeEnd = it->first + it->second;
        if (it->first < end && rangeEnd > start) {
            if (it->first < start) kept.emplace_back(it->first, start - it->first);
            if (rangeEnd > end) kept.emplace_back(end, rangeEnd - end);
            it = watch.available.erase(it);
            continue;
        }
        ++it;
    }
    for (const auto& [rangeStart, bytes] : kept) watch.available[rangeStart] = bytes;
}

}

bool IsGpuVisible(std::uint32_t protect) {
    return (protect & kGraphicsAccess) != 0;
}

void AnnounceAvailable(std::uint64_t start, std::uint64_t bytes) {
    if (bytes == 0) return;
    Watch& watch = State();
    const std::lock_guard guard(watch.lock);
    Remove(watch, start, start + bytes);
    watch.available[start] = bytes;
    if (watch.listener != nullptr) watch.listener(watch.context, kAbxGpuMemoryAvailable, start, bytes);
}

void AnnounceWithdrawn(std::uint64_t start, std::uint64_t bytes) {
    if (bytes == 0) return;
    Watch& watch = State();
    const std::lock_guard guard(watch.lock);
    Remove(watch, start, start + bytes);
    if (watch.listener != nullptr) watch.listener(watch.context, kAbxGpuMemoryWithdrawn, start, bytes);
}

}

ABX_API void AbxXMemWatchGpuMemory(AbxGpuMemoryListener listener, void* context) {
    abx::xmem::Watch& watch = abx::xmem::State();
    const std::lock_guard guard(watch.lock);
    watch.listener = listener;
    watch.context = context;
    if (listener == nullptr) return;
    std::uint64_t runStart = 0;
    std::uint64_t runEnd = 0;
    for (const auto& [start, bytes] : watch.available) {
        if (runEnd != 0 && start == runEnd) {
            runEnd = start + bytes;
            continue;
        }
        if (runEnd != 0) listener(context, kAbxGpuMemoryAvailable, runStart, runEnd - runStart);
        runStart = start;
        runEnd = start + bytes;
    }
    if (runEnd != 0) listener(context, kAbxGpuMemoryAvailable, runStart, runEnd - runStart);
    AbxLog("xmem", "GPU-visible title memory (pages with PAGE_GRAPHICS_* access) is reported to the graphics layer as it is committed, mapped and unmapped; frees through VirtualFree are not seen");
}
