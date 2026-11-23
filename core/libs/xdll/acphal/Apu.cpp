#include "xdll/acphal/acphal.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

namespace {

struct ApuHeap {
    std::mutex lock;
    std::uint8_t* base = nullptr;
    std::uint64_t firstBytes = 0;
    std::uint64_t secondBytes = 0;
};

ApuHeap& Heap() {
    static ApuHeap* heap = new ApuHeap();
    return *heap;
}

}

ABX_API HRESULT ApuCreateHeap(std::uint32_t firstBytes, std::uint32_t secondBytes) {
    ApuHeap& heap = Heap();
    const std::lock_guard guard(heap.lock);
    if (heap.base != nullptr) {
        AbxUnsupported(__func__, "a second audio heap");
    }
    const std::uint64_t total = static_cast<std::uint64_t>(firstBytes) + secondBytes;
    if (total == 0) return E_INVALIDARG;
    heap.base = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, static_cast<SIZE_T>(total), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (heap.base == nullptr) return E_OUTOFMEMORY;
    heap.firstBytes = firstBytes;
    heap.secondBytes = secondBytes;
    AbxLog("acphal", "ApuCreateHeap: %u + %u bytes of audio heap at %p (ordinary host memory; AnyBox has no audio processor)", firstBytes, secondBytes,
           static_cast<void*>(heap.base));
    return S_OK;
}

namespace {

constexpr std::size_t kShapeRequestCount = 0x10;
constexpr std::size_t kShapeContextArray = 0x38;
constexpr std::size_t kShapeContextBytes = 64;

}

ABX_API HRESULT AcpHalAllocateShapeContexts(std::uint8_t* request) {
    if (request == nullptr) return E_POINTER;
    std::uint32_t count = 0;
    std::memcpy(&count, request + kShapeRequestCount, sizeof(count));
    for (std::size_t offset = 0; offset < kShapeRequestCount + 8; offset += 4) {
        std::uint32_t value = 0;
        std::memcpy(&value, request + offset, sizeof(value));
        if (offset != kShapeRequestCount && value != 0) {
            AbxUnsupported(__func__, "a request with fields set besides the measured context count at +0x10");
        }
    }
    void* contexts = count == 0 ? nullptr : VirtualAlloc(nullptr, static_cast<SIZE_T>(count) * kShapeContextBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (count != 0 && contexts == nullptr) return E_OUTOFMEMORY;
    std::memcpy(request + kShapeContextArray, &contexts, sizeof(contexts));
    AbxLog("acphal", "AcpHalAllocateShapeContexts: %u contexts of 64 bytes at %p; nothing processes them, so title audio decoded by the audio processor stays silent", count, contexts);
    return S_OK;
}

namespace {

constexpr std::uint32_t kFirstHeap = 1;
constexpr std::uint32_t kSecondHeap = 2;

std::uint64_t g_firstUsed = 0;
std::uint64_t g_secondUsed = 0;

}

ABX_API HRESULT ApuAlloc(void** address, std::uint32_t* hardwareAddress, std::uint32_t bytes, std::uint32_t alignment, std::uint32_t heapSelector) {
    if (address == nullptr) return E_POINTER;
    *address = nullptr;
    if (hardwareAddress != nullptr) {
        AbxUnsupported(__func__, "a request for the allocation's audio processor address (second argument)");
    }
    if (heapSelector != kFirstHeap && heapSelector != kSecondHeap) {
        AbxUnsupported(__func__, ("heap selector " + std::to_string(heapSelector) + "; only selectors 1 and 2 (the two ApuCreateHeap regions) are known").c_str());
    }
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return E_INVALIDARG;
    ApuHeap& heap = Heap();
    const std::lock_guard guard(heap.lock);
    if (heap.base == nullptr) return E_FAIL;
    const bool first = heapSelector == kFirstHeap;
    std::uint64_t& used = first ? g_firstUsed : g_secondUsed;
    const std::uint64_t limit = first ? heap.firstBytes : heap.secondBytes;
    const std::uint64_t start = (used + alignment - 1) / alignment * alignment;
    if (start + bytes > limit) {
        AbxLog("acphal", "ApuAlloc: %u bytes do not fit in audio heap %u (%llu of %llu bytes used)", bytes, heapSelector, static_cast<unsigned long long>(used),
               static_cast<unsigned long long>(limit));
        return E_OUTOFMEMORY;
    }
    used = start + bytes;
    *address = heap.base + (first ? 0 : heap.firstBytes) + start;
    static bool reported = false;
    if (first && !reported) {
        reported = true;
        AbxLog("acphal", "ApuAlloc selector 1 is served from the first ApuCreateHeap region (inferred: selector 2 measured as the second region)");
    }
    return S_OK;
}

namespace {

constexpr std::size_t kHalSlots = 32;

struct AcpHal {
    const void* const* table;
};

template <std::size_t Slot>
HRESULT HalUnmapped(AcpHal*) {
    AbxUnsupported("acphal object", ("slot " + std::to_string(Slot) + " of the object AcpHalCreate returns is not mapped: no measured call site shows which method it is").c_str());
}

template <std::size_t Slot>
HRESULT HalRecorded(AcpHal*, std::uint64_t first, std::uint64_t second, std::uint64_t third) {
    AbxLog("acphal", "AcpHal object slot %zu (0x%llx, 0x%llx, 0x%llx): recorded and answered S_OK; no audio processor is behind it", Slot,
           static_cast<unsigned long long>(first), static_cast<unsigned long long>(second), static_cast<unsigned long long>(third));
    return S_OK;
}

template <std::size_t... Slots>
std::array<const void*, kHalSlots> HalTable(std::index_sequence<Slots...>) {
    std::array<const void*, kHalSlots> table{reinterpret_cast<const void*>(&HalUnmapped<Slots>)...};
    table[0] = reinterpret_cast<const void*>(&HalRecorded<0>);
    table[2] = reinterpret_cast<const void*>(&HalRecorded<2>);
    return table;
}

const std::array<const void*, kHalSlots> kHalTable = HalTable(std::make_index_sequence<kHalSlots>{});
AcpHal g_hal{kHalTable.data()};

}

ABX_API HRESULT AcpHalCreate(void** hal) {
    if (hal == nullptr) return E_POINTER;
    *hal = &g_hal;
    return S_OK;
}
