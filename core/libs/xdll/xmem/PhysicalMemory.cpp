#include "xdll/xmem/xmem.hpp"
#include "PhysicalMemory.hpp"

#include "GpuVisible.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace abx::xmem {
namespace {

constexpr ULONG kPreservePlaceholder = 0x00000002;
constexpr std::uintptr_t kFramesPerPage = 16;

using VirtualAlloc2Function = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
using MapViewOfFile3Function = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
using UnmapViewOfFile2Function = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);

struct PlaceholderApi {
    VirtualAlloc2Function virtualAlloc2 = nullptr;
    MapViewOfFile3Function mapViewOfFile3 = nullptr;
    UnmapViewOfFile2Function unmapViewOfFile2 = nullptr;
};

const PlaceholderApi& Placeholders() {
    static const PlaceholderApi api = [] {
        PlaceholderApi loaded;
        const HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
        if (kernelbase != nullptr) {
            loaded.virtualAlloc2 = reinterpret_cast<VirtualAlloc2Function>(reinterpret_cast<void*>(GetProcAddress(kernelbase, "VirtualAlloc2")));
            loaded.mapViewOfFile3 = reinterpret_cast<MapViewOfFile3Function>(reinterpret_cast<void*>(GetProcAddress(kernelbase, "MapViewOfFile3")));
            loaded.unmapViewOfFile2 = reinterpret_cast<UnmapViewOfFile2Function>(reinterpret_cast<void*>(GetProcAddress(kernelbase, "UnmapViewOfFile2")));
        }
        if (loaded.virtualAlloc2 == nullptr || loaded.mapViewOfFile3 == nullptr || loaded.unmapViewOfFile2 == nullptr) {
            AbxUnsupported("xmem", "physical page windows need VirtualAlloc2, MapViewOfFile3 and UnmapViewOfFile2 (Windows 10 version 1803 or later)");
        }
        return loaded;
    }();
    return api;
}

struct PageRun {
    std::uintptr_t firstFrame;
    std::size_t count;
    HANDLE section;
};

struct Window {
    std::uintptr_t base;
    std::size_t slots;
    ULONG viewProtection;
    bool gpu;
    std::vector<bool> boundary;
    std::vector<bool> mapped;
};

struct State {
    std::mutex lock;
    std::vector<PageRun> runs;
    std::vector<Window> windows;
    std::uintptr_t nextFrame = kFramesPerPage;
};

State& Physical() {
    static State* state = new State();
    return *state;
}

const PageRun* FindRun(const State& state, std::uintptr_t frame) {
    for (const PageRun& run : state.runs) {
        if (frame >= run.firstFrame && (frame - run.firstFrame) % kFramesPerPage == 0 && (frame - run.firstFrame) / kFramesPerPage < run.count) return &run;
    }
    return nullptr;
}

Window* FindWindow(State& state, std::uintptr_t address) {
    for (Window& window : state.windows) {
        if (address >= window.base && (address - window.base) / kPhysicalPageSize < window.slots) return &window;
    }
    return nullptr;
}

bool IsolateSlot(Window& window, std::size_t slot) {
    if (window.boundary[slot] && window.boundary[slot + 1]) return true;
    void* const address = reinterpret_cast<void*>(window.base + slot * kPhysicalPageSize);
    if (!VirtualFree(address, kPhysicalPageSize, MEM_RELEASE | kPreservePlaceholder)) {
        AbxTrace("splitting the placeholder at %p failed with %lu", address, GetLastError());
        return false;
    }
    window.boundary[slot] = true;
    window.boundary[slot + 1] = true;
    return true;
}

bool UnmapSlot(Window& window, std::size_t slot) {
    if (!window.mapped[slot]) return true;
    void* const address = reinterpret_cast<void*>(window.base + slot * kPhysicalPageSize);
    if (!Placeholders().unmapViewOfFile2(GetCurrentProcess(), address, kPreservePlaceholder)) {
        AbxTrace("unmapping the physical page view at %p failed with %lu", address, GetLastError());
        return false;
    }
    window.mapped[slot] = false;
    return true;
}

bool MapSlot(Window& window, std::size_t slot, const PageRun& run, std::uintptr_t frame) {
    if (!UnmapSlot(window, slot) || !IsolateSlot(window, slot)) return false;
    void* const address = reinterpret_cast<void*>(window.base + slot * kPhysicalPageSize);
    const ULONG64 offset = static_cast<ULONG64>((frame - run.firstFrame) / kFramesPerPage) * kPhysicalPageSize;
    void* const view = Placeholders().mapViewOfFile3(run.section, GetCurrentProcess(), address, offset, kPhysicalPageSize, kReplacePlaceholder, window.viewProtection, nullptr, 0);
    if (view != address) {
        AbxTrace("mapping physical page frame %zu at %p failed with %lu", static_cast<std::size_t>(frame), address, GetLastError());
        return false;
    }
    window.mapped[slot] = true;
    return true;
}

}

void WithdrawMapped(const Window& window, std::size_t firstSlot, std::size_t count) {
    if (!window.gpu) return;
    std::size_t run = 0;
    for (std::size_t slot = firstSlot; slot <= firstSlot + count; ++slot) {
        if (slot < firstSlot + count && window.mapped[slot]) {
            ++run;
            continue;
        }
        if (run != 0) AnnounceWithdrawn(window.base + (slot - run) * kPhysicalPageSize, run * kPhysicalPageSize);
        run = 0;
    }
}

void AnnounceRuns(std::vector<std::uintptr_t>& addresses) {
    std::sort(addresses.begin(), addresses.end());
    std::size_t index = 0;
    while (index < addresses.size()) {
        std::size_t next = index + 1;
        while (next < addresses.size() && addresses[next] == addresses[next - 1] + kPhysicalPageSize) ++next;
        AnnounceAvailable(addresses[index], (next - index) * kPhysicalPageSize);
        index = next;
    }
}

void RegisterWindow(void* base, std::size_t size, std::uint32_t protection, bool gpu) {
    State& state = Physical();
    const std::lock_guard guard(state.lock);
    Window window;
    window.base = reinterpret_cast<std::uintptr_t>(base);
    window.slots = size / kPhysicalPageSize;
    window.viewProtection = protection == PAGE_NOACCESS ? PAGE_READWRITE : protection;
    window.gpu = gpu;
    window.boundary.assign(window.slots + 1, false);
    window.boundary.front() = true;
    window.boundary.back() = true;
    window.mapped.assign(window.slots, false);
    state.windows.push_back(std::move(window));
}

void* ReservePhysicalWindow(std::size_t size, std::uint32_t protection, bool gpu) {
    if (size == 0 || size % kPhysicalPageSize != 0) {
        AbxUnsupported("XMemVirtualAlloc", "a physical page window whose size is not a whole number of 64 KiB pages");
    }
    void* const base = Placeholders().virtualAlloc2(GetCurrentProcess(), nullptr, size, MEM_RESERVE | kReservePlaceholder, PAGE_NOACCESS, nullptr, 0);
    if (base == nullptr) return nullptr;
    RegisterWindow(base, size, protection, gpu);
    return base;
}

void* AdoptPhysicalWindow(void* address, std::size_t size, std::uint32_t protection, bool gpu) {
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(address);
    if (size == 0 || size % kPhysicalPageSize != 0 || start % kPhysicalPageSize != 0) {
        AbxUnsupported("XMemVirtualAlloc", "a physical page window over a placeholder that is not 64 KiB aligned or sized");
    }
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(address, &region, sizeof(region)) != sizeof(region) || region.BaseAddress != address || region.State != MEM_RESERVE || region.RegionSize != size) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return nullptr;
    }
    {
        State& state = Physical();
        const std::lock_guard guard(state.lock);
        if (FindWindow(state, start) != nullptr) {
            AbxUnsupported("XMemVirtualAlloc", "a physical page window over a range that already is one");
        }
    }
    RegisterWindow(address, size, protection, gpu);
    return address;
}

void* AllocateWithPlaceholders(void* address, std::size_t size, std::uint32_t type, std::uint32_t protection) {
    return Placeholders().virtualAlloc2(GetCurrentProcess(), address, size, type, protection, nullptr, 0);
}

}

using abx::xmem::kPhysicalPageSize;

ABX_API BOOL XMemAllocatePhysicalPages(std::uint64_t attributes, std::uintptr_t* pageCount, std::uintptr_t* pageArray) {
    if (pageCount == nullptr || pageArray == nullptr || *pageCount == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const std::uintptr_t count = *pageCount;
    if (count > (std::numeric_limits<std::uint64_t>::max)() / kPhysicalPageSize) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(count) * kPhysicalPageSize;
    const HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), nullptr);
    if (section == nullptr) {
        const DWORD error = GetLastError();
        AbxTrace("XMemAllocatePhysicalPages(0x%llx, %zu pages): the backing section could not be created (%lu)", static_cast<unsigned long long>(attributes),
                 static_cast<std::size_t>(count), error);
        *pageCount = 0;
        SetLastError(error);
        return FALSE;
    }
    auto& state = abx::xmem::Physical();
    std::uintptr_t first = 0;
    {
        const std::lock_guard guard(state.lock);
        first = state.nextFrame;
        try {
            state.runs.push_back({first, count, section});
        } catch (const std::bad_alloc&) {
            CloseHandle(section);
            *pageCount = 0;
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
        state.nextFrame += count * abx::xmem::kFramesPerPage;
    }
    for (std::uintptr_t index = 0; index < count; ++index) {
        pageArray[index] = first + index * abx::xmem::kFramesPerPage;
    }
    AbxTrace("XMemAllocatePhysicalPages(0x%llx, %zu pages of 64 KiB) -> frames %zu..%zu", static_cast<unsigned long long>(attributes), static_cast<std::size_t>(count),
             static_cast<std::size_t>(first), static_cast<std::size_t>(first + count - 1));
    return TRUE;
}

ABX_API void* XMemMapPhysicalPages(void* address, std::size_t pageCount, const std::uintptr_t* pageArray) {
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(address);
    if (start % kPhysicalPageSize != 0) {
        AbxUnsupported(__func__, "a target address that is not 64 KiB aligned");
    }
    auto& state = abx::xmem::Physical();
    const std::lock_guard guard(state.lock);
    abx::xmem::Window* const window = abx::xmem::FindWindow(state, start);
    if (window == nullptr) {
        AbxUnsupported(__func__, "a target address outside every physical page window reserved through XMemVirtualAlloc");
    }
    const std::size_t firstSlot = (start - window->base) / kPhysicalPageSize;
    if (pageCount > window->slots - firstSlot) {
        AbxUnsupported(__func__, "a page range that runs past the end of its physical page window");
    }
    abx::xmem::WithdrawMapped(*window, firstSlot, pageCount);
    for (std::size_t index = 0; index < pageCount; ++index) {
        const std::size_t slot = firstSlot + index;
        if (pageArray == nullptr) {
            if (!abx::xmem::UnmapSlot(*window, slot)) return nullptr;
            continue;
        }
        const abx::xmem::PageRun* const run = abx::xmem::FindRun(state, pageArray[index]);
        if (run == nullptr) {
            AbxUnsupported(__func__, "a page frame number that XMemAllocatePhysicalPages did not return");
        }
        if (!abx::xmem::MapSlot(*window, slot, *run, pageArray[index])) return nullptr;
    }
    if (window->gpu && pageArray != nullptr && pageCount != 0) abx::xmem::AnnounceAvailable(start, pageCount * kPhysicalPageSize);
    AbxTrace("XMemMapPhysicalPages(%p, %zu, %s)", address, pageCount, pageArray == nullptr ? "unmap" : "map");
    return address;
}

ABX_API BOOL XMemMapPhysicalPagesScatter(void* const* addresses, std::size_t count, const std::uintptr_t* pageArray) {
    if (addresses == nullptr || pageArray == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    auto& state = abx::xmem::Physical();
    const std::lock_guard guard(state.lock);
    std::vector<std::uintptr_t> mapped;
    for (std::size_t index = 0; index < count; ++index) {
        const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(addresses[index]);
        if (start % kPhysicalPageSize != 0) {
            AbxUnsupported(__func__, "a target address that is not 64 KiB aligned");
        }
        abx::xmem::Window* const window = abx::xmem::FindWindow(state, start);
        if (window == nullptr) {
            AbxUnsupported(__func__, "a target address outside every physical page window");
        }
        const std::size_t slot = (start - window->base) / kPhysicalPageSize;
        abx::xmem::WithdrawMapped(*window, slot, 1);
        if (pageArray[index] == 0) {
            if (!abx::xmem::UnmapSlot(*window, slot)) return FALSE;
            continue;
        }
        const abx::xmem::PageRun* const run = abx::xmem::FindRun(state, pageArray[index]);
        if (run == nullptr) {
            AbxUnsupported(__func__, "a page frame number that XMemAllocatePhysicalPages did not return");
        }
        if (!abx::xmem::MapSlot(*window, slot, *run, pageArray[index])) return FALSE;
        if (window->gpu) mapped.push_back(start);
    }
    abx::xmem::AnnounceRuns(mapped);
    AbxTrace("XMemMapPhysicalPagesScatter(%zu pages)", count);
    return TRUE;
}
