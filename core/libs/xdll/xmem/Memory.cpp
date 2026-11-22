#include "xdll/xmem/xmem.hpp"

#include <malloc.h>

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

ABX_API void* XMemAlloc(std::size_t size, std::uint64_t attributes) {
    if (size == 0) {
        AbxUnsupported(__func__, "size is 0");
    }
    void* const address = Allocate(size);
    AbxTrace("XMemAlloc(%zu, 0x%llx) -> %p", size, static_cast<unsigned long long>(attributes), address);
    return address;
}

ABX_API void XMemFree(void* address, std::uint64_t attributes) {
    AbxTrace("XMemFree(%p, 0x%llx)", address, static_cast<unsigned long long>(attributes));
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
        AbxUnsupported(__func__, "address was not returned by XMemAlloc");
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
