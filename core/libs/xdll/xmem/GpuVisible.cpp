#include "GpuVisible.hpp"

#include "xdll/xmem/xmem.hpp"

#include <windows.h>

#include <map>
#include <mutex>
#include <vector>

namespace abx::xmem {

namespace {

constexpr std::uint32_t kGraphicsAccess = PAGE_GRAPHICS_READONLY | PAGE_GRAPHICS_READWRITE | PAGE_GRAPHICS_EXECUTE | PAGE_GRAPHICS_EXECUTE_READ | PAGE_GRAPHICS_EXECUTE_READWRITE;

struct Span {
    std::uint64_t bytes = 0;
    std::uint64_t source = 0;
};

struct Watch {
    std::recursive_mutex lock;
    std::map<std::uint64_t, Span> available;
    std::map<std::uint64_t, std::uint64_t> backings;
    AbxGpuMemoryListener listener = nullptr;
    void* context = nullptr;
};

Watch& State() {
    static Watch* watch = new Watch();
    return *watch;
}

void Remove(Watch& watch, std::uint64_t start, std::uint64_t end) {
    std::vector<std::pair<std::uint64_t, Span>> kept;
    auto it = watch.available.upper_bound(start);
    if (it != watch.available.begin()) --it;
    while (it != watch.available.end() && it->first < end) {
        const std::uint64_t rangeEnd = it->first + it->second.bytes;
        if (rangeEnd > start) {
            const Span span = it->second;
            if (it->first < start) kept.emplace_back(it->first, Span{start - it->first, span.source});
            if (rangeEnd > end) kept.emplace_back(end, Span{rangeEnd - end, span.source == 0 ? 0 : span.source + (end - it->first)});
            it = watch.available.erase(it);
            continue;
        }
        ++it;
    }
    for (const auto& [rangeStart, span] : kept) watch.available[rangeStart] = span;
}

void Notify(Watch& watch, std::uint32_t event, std::uint64_t start, std::uint64_t bytes, std::uint64_t source) {
    if (watch.listener != nullptr) watch.listener(watch.context, event, start, bytes, source);
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
    watch.available[start] = Span{bytes, 0};
    Notify(watch, kAbxGpuMemoryAvailable, start, bytes, 0);
}

void AnnounceWithdrawn(std::uint64_t start, std::uint64_t bytes) {
    if (bytes == 0) return;
    Watch& watch = State();
    const std::lock_guard guard(watch.lock);
    Remove(watch, start, start + bytes);
    Notify(watch, kAbxGpuMemoryWithdrawn, start, bytes, 0);
}

void AnnounceBacking(std::uint64_t view, std::uint64_t bytes) {
    if (bytes == 0) return;
    Watch& watch = State();
    const std::lock_guard guard(watch.lock);
    watch.backings[view] = bytes;
    Notify(watch, kAbxGpuMemoryBackingAvailable, view, bytes, 0);
}

void AnnounceBackingWithdrawn(std::uint64_t view, std::uint64_t bytes) {
    if (bytes == 0) return;
    Watch& watch = State();
    const std::lock_guard guard(watch.lock);
    watch.backings.erase(view);
    Notify(watch, kAbxGpuMemoryBackingWithdrawn, view, bytes, 0);
}

void AnnounceAliased(std::uint64_t start, std::uint64_t bytes, std::uint64_t source) {
    if (bytes == 0) return;
    Watch& watch = State();
    const std::lock_guard guard(watch.lock);
    Remove(watch, start, start + bytes);
    watch.available[start] = Span{bytes, source};
    Notify(watch, kAbxGpuMemoryAliased, start, bytes, source);
}

}

ABX_API void AbxXMemAnnounceWritten(std::uint64_t start, std::uint64_t bytes) {
    if (bytes == 0) return;
    abx::xmem::Watch& watch = abx::xmem::State();
    const std::lock_guard guard(watch.lock);
    abx::xmem::Notify(watch, kAbxGpuMemoryWritten, start, bytes, 0);
}

ABX_API void AbxXMemWatchGpuMemory(AbxGpuMemoryListener listener, void* context) {
    abx::xmem::Watch& watch = abx::xmem::State();
    const std::lock_guard guard(watch.lock);
    watch.listener = listener;
    watch.context = context;
    if (listener == nullptr) return;
    for (const auto& [view, bytes] : watch.backings) listener(context, kAbxGpuMemoryBackingAvailable, view, bytes, 0);
    std::uint64_t runStart = 0;
    std::uint64_t runEnd = 0;
    for (const auto& [start, span] : watch.available) {
        if (span.source != 0) {
            listener(context, kAbxGpuMemoryAliased, start, span.bytes, span.source);
            continue;
        }
        if (runEnd != 0 && start == runEnd) {
            runEnd = start + span.bytes;
            continue;
        }
        if (runEnd != 0) listener(context, kAbxGpuMemoryAvailable, runStart, runEnd - runStart, 0);
        runStart = start;
        runEnd = start + span.bytes;
    }
    if (runEnd != 0) listener(context, kAbxGpuMemoryAvailable, runStart, runEnd - runStart, 0);
    AbxLog("xmem",
           "GPU-visible title memory (pages with PAGE_GRAPHICS_* access) is reported to the graphics layer as it is committed, mapped and unmapped; physical pages are "
           "reported once per allocation and mapped as aliases of it; frees through VirtualFree are not seen");
}
