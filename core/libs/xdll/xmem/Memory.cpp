#include "xdll/xmem/xmem.hpp"
#include "PhysicalMemory.hpp"
#include "GpuVisible.hpp"

#include <malloc.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <unordered_map>

namespace {

constexpr std::size_t HeapAlignment = 64;
constexpr std::size_t PageThreshold = 64 * 1024;

enum class Block { Heap, Pages };

std::mutex& BlocksLock() {
    static std::mutex lock;
    return lock;
}

std::unordered_map<void*, Block>& Blocks() {
    static std::unordered_map<void*, Block> blocks;
    return blocks;
}

void Release(void* address, Block block) {
    if (block == Block::Pages) {
        VirtualFree(address, 0, MEM_RELEASE);
    } else {
        _aligned_free(address);
    }
}

void* Allocate(std::size_t size) {
    void* address = nullptr;
    Block block = Block::Heap;
    if (size >= PageThreshold) {
        address = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        block = Block::Pages;
    } else {
        address = _aligned_malloc(size, HeapAlignment);
        if (address != nullptr) {
            std::memset(address, 0, size);
        }
    }
    if (address == nullptr) {
        return nullptr;
    }
    try {
        const std::lock_guard lock(BlocksLock());
        Blocks().emplace(address, block);
    } catch (const std::bad_alloc&) {
        Release(address, block);
        return nullptr;
    }
    return address;
}

}

namespace {

struct TitleAllocationHooks {
    void* (*allocate)(std::size_t size, std::uint64_t attributes);
    void (*release)(void* address, std::uint64_t attributes);
};

const TitleAllocationHooks* TitleHooks() {
    static const TitleAllocationHooks* hooks = [] {
        const auto* found = reinterpret_cast<const TitleAllocationHooks*>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), "__XMemAllocationHooks__")));
        AbxLog("xmem", found != nullptr ? "the title exports __XMemAllocationHooks__: XMemAlloc and XMemFree go through its routines"
                                        : "the title exports no __XMemAllocationHooks__: XMemAlloc and XMemFree use the default allocator");
        return found;
    }();
    return hooks;
}

}

ABX_API void* XMemAlloc(std::size_t size, std::uint64_t attributes) {
    const TitleAllocationHooks* hooks = TitleHooks();
    if (hooks != nullptr && hooks->allocate != nullptr) {
        void* const address = hooks->allocate(size, attributes);
        AbxTrace("XMemAlloc(%zu, 0x%llx) -> %p through the title's hook", size, static_cast<unsigned long long>(attributes), address);
        return address;
    }
    return XMemAllocDefault(size, attributes);
}

ABX_API void XMemFree(void* address, std::uint64_t attributes) {
    const TitleAllocationHooks* hooks = TitleHooks();
    if (hooks != nullptr && hooks->release != nullptr) {
        AbxTrace("XMemFree(%p, 0x%llx) through the title's hook", address, static_cast<unsigned long long>(attributes));
        hooks->release(address, attributes);
        return;
    }
    XMemFreeDefault(address, attributes);
}

ABX_API void* XMemAllocDefault(std::size_t size, std::uint64_t attributes) {
    if (size == 0) {
        AbxUnsupported(__func__, "size is 0");
    }
    void* const address = Allocate(size);
    AbxTrace("XMemAllocDefault(%zu, 0x%llx) -> %p", size, static_cast<unsigned long long>(attributes), address);
    return address;
}

ABX_API void XMemFreeDefault(void* address, std::uint64_t attributes) {
    AbxTrace("XMemFreeDefault(%p, 0x%llx)", address, static_cast<unsigned long long>(attributes));
    if (address == nullptr) {
        AbxUnsupported(__func__, "address is null");
    }
    Block block = Block::Heap;
    bool known = false;
    {
        const std::lock_guard lock(BlocksLock());
        const auto found = Blocks().find(address);
        if (found != Blocks().end()) {
            block = found->second;
            Blocks().erase(found);
            known = true;
        }
    }
    if (!known) {
        AbxUnsupported(__func__, "address was not returned by XMemAllocDefault");
    }
    Release(address, block);
}

ABX_API BOOL XMemVirtualQuery(HANDLE process, const void* address, XMemVirtualQueryInfo* info) {
    if (info == nullptr) {
        AbxUnsupported(__func__, "info is null");
    }
    std::memset(info, 0, sizeof(*info));
    const SIZE_T written = VirtualQueryEx(process, address, &info->basic, sizeof(info->basic));
    const BOOL result = written == sizeof(info->basic) ? TRUE : FALSE;
    AbxTrace("XMemVirtualQuery(%p, %p, %p) -> %d, base %p, size 0x%zx, state 0x%lx, protect 0x%lx", process, address, static_cast<void*>(info), result,
             info->basic.BaseAddress, static_cast<std::size_t>(info->basic.RegionSize), info->basic.State, info->basic.Protect);
    return result;
}

ABX_API BOOL XMemGetWorkingSetStatistics(std::uint32_t request, void* statistics) {
    AbxTrace("XMemGetWorkingSetStatistics(0x%x, %p) -> 0", request, statistics);
    return FALSE;
}

namespace {

constexpr std::uint32_t kPlaceholderTypes = abx::xmem::kReservePlaceholder | abx::xmem::kReplacePlaceholder;
constexpr std::uint32_t kDesktopAllocationTypes = MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN | kPlaceholderTypes;
constexpr std::uint32_t kPhysicalAllocationType = 0x00400000u;
constexpr std::uint32_t kConsoleOnlyAllocationTypes = kPhysicalAllocationType | 0x20000000u;
constexpr std::uint32_t kProtectionValues[] = {PAGE_NOACCESS, PAGE_READONLY, PAGE_READWRITE, PAGE_WRITECOPY, PAGE_EXECUTE, PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE,
                                               PAGE_EXECUTE_WRITECOPY};

bool StandardProtection(std::uint32_t protection) {
    for (const std::uint32_t value : kProtectionValues) {
        if (protection == value) return true;
    }
    return false;
}

}

ABX_API void* XMemVirtualAlloc(void* address, std::size_t size, std::uint32_t allocationType, std::uint64_t attributes, std::uint32_t protect) {
    const std::uint32_t unknownTypes = allocationType & ~(kDesktopAllocationTypes | kConsoleOnlyAllocationTypes);
    if (unknownTypes != 0) {
        char message[160];
        std::snprintf(message, sizeof(message), "allocation type 0x%x has bits 0x%x that are neither desktop nor known console-only flags (protect 0x%x, attributes 0x%llx)",
                      allocationType, unknownTypes, protect, static_cast<unsigned long long>(attributes));
        AbxUnsupported(__func__, message);
    }
    const std::uint32_t protection = protect & 0xFFu;
    if (!StandardProtection(protection)) {
        char message[128];
        std::snprintf(message, sizeof(message), "page protection 0x%x: its low byte is not one standard protection value (type 0x%x)", protect, allocationType);
        AbxUnsupported(__func__, message);
    }
    if ((allocationType & kPhysicalAllocationType) != 0 && (allocationType & MEM_COMMIT) == 0) {
        if ((allocationType & abx::xmem::kReservePlaceholder) != 0) {
            AbxUnsupported(__func__, "a physical page window that is itself reserved as a placeholder");
        }
        void* window = nullptr;
        if ((allocationType & abx::xmem::kReplacePlaceholder) != 0) {
            window = abx::xmem::AdoptPhysicalWindow(address, size, protection, abx::xmem::IsGpuVisible(protect));
        } else if (address == nullptr) {
            window = abx::xmem::ReservePhysicalWindow(size, protection, abx::xmem::IsGpuVisible(protect));
        } else {
            AbxUnsupported(__func__, "a physical page window at a fixed address that is not a placeholder");
        }
        AbxTrace("XMemVirtualAlloc(%p, 0x%zx, type 0x%x, attributes 0x%llx, protect 0x%x) = %p (physical page window, views mapped with protection 0x%x)", address, size,
                 allocationType, static_cast<unsigned long long>(attributes), protect, window, protection);
        return window;
    }
    void* result = (allocationType & kPlaceholderTypes) != 0 ? abx::xmem::AllocateWithPlaceholders(address, size, allocationType & kDesktopAllocationTypes, protection)
                                                             : VirtualAlloc(address, size, allocationType & kDesktopAllocationTypes, protection);
    if (result != nullptr && (allocationType & MEM_COMMIT) != 0 && abx::xmem::IsGpuVisible(protect)) {
        abx::xmem::AnnounceAvailable(reinterpret_cast<std::uint64_t>(result), size);
    }
    AbxTrace("XMemVirtualAlloc(%p, 0x%zx, type 0x%x, attributes 0x%llx, protect 0x%x) = %p (console-only type bits 0x%x and protection bits 0x%x dropped)", address, size,
             allocationType, static_cast<unsigned long long>(attributes), protect, result, allocationType & kConsoleOnlyAllocationTypes, protect & ~0xFFu);
    return result;
}
