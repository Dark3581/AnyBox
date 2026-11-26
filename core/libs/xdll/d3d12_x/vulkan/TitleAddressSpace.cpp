#include "TitleAddressSpace.hpp"

#include "FrameTiming.hpp"

#include "TitleBufferWindow.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace abx::d3d12x {

namespace {

constexpr VkBufferUsageFlags kTableUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
constexpr VkBufferUsageFlags kImportUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

std::uint64_t AlignDown(std::uint64_t value, std::uint64_t alignment) {
    return value / alignment * alignment;
}

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

constexpr std::uint64_t kAliasReservationBytes = 1ull << 30;
constexpr std::uint64_t kBackingChunkBytes = 256ull << 20;
constexpr std::uint64_t kVideoBlockBytes = 32ull << 20;
constexpr std::uint64_t kMoveWaitNanoseconds = 250'000'000ull;
constexpr std::uint64_t kReturnWaitNanoseconds = 30'000'000'000ull;
constexpr std::size_t kVideoPageEvents = 256;
constexpr auto kBusyPause = std::chrono::seconds(20);

constexpr std::uint64_t kMoveRoundBytes = 32ull << 20;
constexpr std::size_t kGpuCopyPages = 3;
constexpr std::uint64_t kReadRoundBytes = 8ull << 20;
constexpr std::size_t kReadRoundLooks = 4096;
constexpr auto kReadRoomPause = std::chrono::seconds(5);
constexpr auto kReadWaitingCount = std::chrono::seconds(30);
constexpr std::uint64_t kReadChecksAlways = 64;
constexpr std::uint64_t kReadCheckEvery = 8;
constexpr std::uint64_t kReadCheckEveryRead = 64;
constexpr std::uint64_t kReadDifferencesAllowed = 16;
constexpr std::uint64_t kReadWriteEventsAlways = 16;
constexpr int kReadNow = 0;
constexpr int kReadNever = 1;
constexpr int kReadLater = 2;
constexpr int kReadPinned = 3;
constexpr int kReadWritable = 4;

constexpr std::uint8_t kMarkEpochs = 0x3F;
constexpr std::uint8_t kMarkHeld = 0x40;
constexpr std::uint8_t kMarkRootWrites = 0x80;
constexpr std::uint32_t kMarkNever = 0xFFFFFFFFu;
constexpr std::uint32_t kUsedEpochs = 4;
constexpr std::uint32_t kStaleEpochs = 8;
constexpr std::uint64_t kMarkSpanBytes = 16ull << 20;
constexpr std::uint64_t kRootWordMarkBytes = 2 * kTitlePageBytes;
constexpr std::uint64_t kTitleAddressLimit = kTitleRegionCount * kTitleRegionBytes;
constexpr std::size_t kOwnRoundLooks = 4096;

std::uint32_t MarkAge(std::uint8_t mark, std::uint8_t epoch) {
    const std::uint32_t at = mark & kMarkEpochs;
    if (at == 0) return kMarkNever;
    return (epoch + kMarkEpochs - at) % kMarkEpochs;
}

std::atomic<TitleAddressSpace*> g_guarded{nullptr};

thread_local const char* t_namingCommand = nullptr;

constexpr std::size_t kActivities = 64;
constexpr const char* kWaitsForLock = "waits for the address space lock";
constexpr const char* kHoldsLock = "holds the address space lock";
constexpr const char* kWaitsForQueueLock = "holds the address space lock and waits for the queue lock";
constexpr const char* kWaitsForGpu = "holds the address space and queue locks and waits for the GPU to finish what it has";
constexpr const char* kWaitsForIdleQueue = "holds the address space and queue locks and waits for the GPU queue to be idle (no time limit)";
constexpr const char* kWaitsForCopy = "holds the address space and queue locks and waits for the GPU's copy of the pages back into system memory (no time limit)";

struct Activity {
    std::atomic<std::uint32_t> thread{0};
    std::atomic<const char*> what{nullptr};
    std::atomic<const char*> stage{nullptr};
    std::atomic<std::uint64_t> address{0};
    std::atomic<std::uint64_t> since{0};
    std::atomic<std::uint64_t> order{0};
};

Activity g_activities[kActivities];
std::atomic<std::uint64_t> g_activityOrder{0};
std::atomic<std::uint64_t> g_activitiesUnlisted{0};

class Doing {
public:
    Doing(const char* what, std::uint64_t address) {
        const std::uint32_t thread = GetCurrentThreadId();
        for (Activity& slot : g_activities) {
            std::uint32_t unused = 0;
            if (!slot.thread.compare_exchange_strong(unused, thread, std::memory_order_acq_rel)) continue;
            slot.address.store(address, std::memory_order_relaxed);
            slot.since.store(GetTickCount64(), std::memory_order_relaxed);
            slot.stage.store(kWaitsForLock, std::memory_order_relaxed);
            slot.what.store(what, std::memory_order_relaxed);
            slot.order.store(g_activityOrder.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_release);
            slot_ = &slot;
            return;
        }
        g_activitiesUnlisted.fetch_add(1, std::memory_order_relaxed);
    }
    ~Doing() {
        if (slot_ == nullptr) return;
        slot_->order.store(0, std::memory_order_relaxed);
        slot_->what.store(nullptr, std::memory_order_relaxed);
        slot_->thread.store(0, std::memory_order_release);
    }
    Doing(const Doing&) = delete;
    Doing& operator=(const Doing&) = delete;
    void Stage(const char* stage) {
        if (slot_ != nullptr) slot_->stage.store(stage, std::memory_order_relaxed);
    }

private:
    Activity* slot_ = nullptr;
};

void StageOfThisThread(const char* stage) {
    const std::uint32_t thread = GetCurrentThreadId();
    Activity* newest = nullptr;
    std::uint64_t order = 0;
    for (Activity& slot : g_activities) {
        if (slot.thread.load(std::memory_order_acquire) != thread) continue;
        const std::uint64_t claimed = slot.order.load(std::memory_order_acquire);
        if (claimed <= order) continue;
        order = claimed;
        newest = &slot;
    }
    if (newest != nullptr) newest->stage.store(stage, std::memory_order_relaxed);
}

struct AccessCounts {
    std::atomic<std::uint64_t> resumed{0};
    std::atomic<std::uint64_t> passedOn{0};
    std::atomic<std::uint64_t> lastAddress{0};
    std::atomic<std::uint64_t> lastCode{0};
    std::atomic<std::uint64_t> lastTick{0};
    std::atomic<std::uint32_t> lastThread{0};
    std::atomic<std::uint64_t> repeats{0};
    std::atomic<std::uint64_t> mostRepeats{0};
    std::atomic<std::uint64_t> mostAddress{0};
    std::atomic<std::uint64_t> mostCode{0};
    std::atomic<std::uint32_t> mostThread{0};
};

AccessCounts g_accesses;

constexpr std::size_t kAccessRows = 64;

struct AccessRow {
    std::atomic<std::uint32_t> thread{0};
    std::atomic<std::uint64_t> address{0};
    std::atomic<std::uint64_t> code{0};
    std::atomic<std::uint64_t> repeats{0};
};

AccessRow g_accessRows[kAccessRows];

std::atomic<std::uint64_t> g_systemCalls{0};
std::atomic<std::uint64_t> g_systemCallViews{0};

bool Reachable(std::uint64_t address, bool write) {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return !write || (region.Protect & kWritable) != 0;
}

void CountAccess(bool resumed, std::uint64_t address, std::uint64_t code) {
    AccessCounts& counts = g_accesses;
    if (!resumed) {
        counts.passedOn.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::uint32_t thread = GetCurrentThreadId();
    AccessRow& row = g_accessRows[thread % kAccessRows];
    const bool same = row.thread.load(std::memory_order_relaxed) == thread && row.address.load(std::memory_order_relaxed) == address &&
                      row.code.load(std::memory_order_relaxed) == code;
    const std::uint64_t repeats = same ? row.repeats.load(std::memory_order_relaxed) + 1 : 1;
    row.thread.store(thread, std::memory_order_relaxed);
    row.address.store(address, std::memory_order_relaxed);
    row.code.store(code, std::memory_order_relaxed);
    row.repeats.store(repeats, std::memory_order_relaxed);
    counts.repeats.store(repeats, std::memory_order_relaxed);
    counts.lastThread.store(thread, std::memory_order_relaxed);
    counts.lastAddress.store(address, std::memory_order_relaxed);
    counts.lastCode.store(code, std::memory_order_relaxed);
    counts.lastTick.store(GetTickCount64(), std::memory_order_relaxed);
    counts.resumed.fetch_add(1, std::memory_order_relaxed);
    if (repeats > counts.mostRepeats.load(std::memory_order_relaxed)) {
        counts.mostRepeats.store(repeats, std::memory_order_relaxed);
        counts.mostAddress.store(address, std::memory_order_relaxed);
        counts.mostCode.store(code, std::memory_order_relaxed);
        counts.mostThread.store(thread, std::memory_order_relaxed);
    }
}

LONG CALLBACK OnGuardedPage(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    TitleAddressSpace* const space = g_guarded.load(std::memory_order_acquire);
    if (space == nullptr) return EXCEPTION_CONTINUE_SEARCH;
    const ULONG_PTR access = record->ExceptionInformation[0];
    const std::uint64_t address = record->ExceptionInformation[1];
    if (access > 1) return EXCEPTION_CONTINUE_SEARCH;
    const std::uint64_t code = reinterpret_cast<std::uint64_t>(record->ExceptionAddress);
    const bool resumed = space->ReturnForAccess(address, code, access == 1);
    CountAccess(resumed, address, code);
    return resumed ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

}

std::unique_ptr<TitleAddressSpace> TitleAddressSpace::Create(const VulkanDevice& device, std::string& error) {
    std::unique_ptr<TitleAddressSpace> space(new TitleAddressSpace(device));
    if (!space->Initialize(error)) return nullptr;
    return space;
}

TitleAddressSpace::~TitleAddressSpace() {
    marking_.store(false, std::memory_order_release);
    if (video_) {
        TitleAddressSpace* self = this;
        g_guarded.compare_exchange_strong(self, nullptr);
        {
            std::lock_guard<std::mutex> guard(lock_);
            std::vector<std::uint64_t> sources;
            for (const auto& [source, page] : video_->moved) sources.push_back(source);
            ReturnPages(sources, PageReturn::Unmapped);
        }
        std::lock_guard<std::mutex> queue(device_.QueueLock());
        if (video_->freedFence != VK_NULL_HANDLE) {
            device_.Vk().waitForFences(device_.Handle(), 1, &video_->freedFence, VK_TRUE, kReturnWaitNanoseconds);
            device_.Vk().destroyFence(device_.Handle(), video_->freedFence, nullptr);
        }
        for (VkFence fence : video_->lateFences) {
            device_.Vk().waitForFences(device_.Handle(), 1, &fence, VK_TRUE, kReturnWaitNanoseconds);
            device_.Vk().destroyFence(device_.Handle(), fence, nullptr);
        }
        if (video_->useFence != VK_NULL_HANDLE) {
            device_.Vk().waitForFences(device_.Handle(), 1, &video_->useFence, VK_TRUE, kReturnWaitNanoseconds);
            device_.Vk().destroyFence(device_.Handle(), video_->useFence, nullptr);
        }
        if (video_->usePool != VK_NULL_HANDLE) device_.Vk().destroyCommandPool(device_.Handle(), video_->usePool, nullptr);
        if (video_->pool != VK_NULL_HANDLE) device_.Vk().destroyCommandPool(device_.Handle(), video_->pool, nullptr);
        for (VideoBlock& block : video_->blocks) Destroy(block.allocation);
    }
    for (auto& [start, range] : ranges_) Destroy(range.imported);
    for (auto& [start, chunk] : chunks_) Destroy(chunk);
    for (Allocation& region : regions_) Destroy(region);
    if (aliasReservation_ != nullptr) VirtualFree(aliasReservation_, 0, MEM_RELEASE);
    Destroy(root_);
    Destroy(nullRegion_);
    Destroy(nullPage_);
}

void TitleAddressSpace::Destroy(Allocation& allocation) {
    const DeviceFunctions& vk = device_.Vk();
    if (allocation.buffer != VK_NULL_HANDLE) vk.destroyBuffer(device_.Handle(), allocation.buffer, nullptr);
    if (allocation.memory != VK_NULL_HANDLE) vk.freeMemory(device_.Handle(), allocation.memory, nullptr);
    allocation = Allocation{};
}

bool TitleAddressSpace::CreateTable(std::uint64_t bytes, Allocation& table, std::string& error) {
    const DeviceFunctions& vk = device_.Vk();
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = bytes;
    bufferInfo.usage = kTableUsage;
    if (vk.createBuffer(device_.Handle(), &bufferInfo, nullptr, &table.buffer) != VK_SUCCESS) {
        table.buffer = VK_NULL_HANDLE;
        error = "vkCreateBuffer failed for a title page table";
        return false;
    }
    VkMemoryRequirements requirements{};
    vk.getBufferMemoryRequirements(device_.Handle(), table.buffer, &requirements);
    std::optional<std::uint32_t> type =
        device_.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!type) type = device_.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!type) {
        error = "no host-visible coherent memory type holds the title page tables";
        return false;
    }
    VkMemoryPriorityAllocateInfoEXT priority{VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT};
    priority.priority = 1.0f;
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    if (device_.PageableMemory()) flags.pNext = &priority;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = *type;
    if (vk.allocateMemory(device_.Handle(), &allocate, nullptr, &table.memory) != VK_SUCCESS) {
        table.memory = VK_NULL_HANDLE;
        error = "vkAllocateMemory failed for a title page table";
        return false;
    }
    if (vk.bindBufferMemory(device_.Handle(), table.buffer, table.memory, 0) != VK_SUCCESS ||
        vk.mapMemory(device_.Handle(), table.memory, 0, VK_WHOLE_SIZE, 0, &table.mapped) != VK_SUCCESS) {
        error = "a title page table could not be bound or mapped";
        return false;
    }
    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = table.buffer;
    table.address = vk.getBufferDeviceAddress(device_.Handle(), &addressInfo);
    if (table.address == 0) {
        error = "a title page table has no device address";
        return false;
    }
    return true;
}

bool TitleAddressSpace::Initialize(std::string& error) {
    if (device_.HostPointerProperties() == nullptr || !device_.BufferDeviceAddress()) {
        error = "the device was created without host memory import or buffer device addresses";
        return false;
    }
    if (!CreateTable(2 * kTitlePageBytes, nullPage_, error) || !CreateTable(kTitlePagesPerRegion * sizeof(std::uint64_t), nullRegion_, error) ||
        !CreateTable(kTitleRegionCount * sizeof(std::uint64_t), root_, error)) {
        return false;
    }
    std::memset(nullPage_.mapped, 0, 2 * kTitlePageBytes);
    std::fill_n(static_cast<std::uint64_t*>(nullRegion_.mapped), kTitlePagesPerRegion, nullPage_.address);
    std::fill_n(static_cast<std::uint64_t*>(root_.mapped), kTitleRegionCount, nullRegion_.address);
    regions_.resize(kTitleRegionCount);
    const std::optional<std::uint64_t> low = FindFreeRange(kAliasReservationBytes, kTitlePageBytes, kLowWindowStart, kLowWindowEnd);
    if (low) aliasReservation_ = VirtualAlloc(reinterpret_cast<void*>(*low), static_cast<SIZE_T>(kAliasReservationBytes), MEM_RESERVE, PAGE_NOACCESS);
    if (aliasReservation_ != nullptr) {
        aliasBase_ = reinterpret_cast<std::uint64_t>(aliasReservation_);
        aliasBytes_ = kAliasReservationBytes;
    }
    return true;
}

void TitleAddressSpace::PointAliases() {
    for (const auto& [slot, alias] : aliases_) {
        auto it = ranges_.upper_bound(alias.start);
        bool pointed = false;
        if (it != ranges_.begin()) {
            --it;
            const std::uint64_t offset = alias.start - it->first;
            if (offset <= it->second.bytes && alias.bytes <= it->second.bytes - offset) {
                Point(slot, AlignUp(alias.bytes, kTitlePageBytes), it->second.device + offset, slot);
                pointed = true;
            }
        }
        if (!pointed) Clear(slot, AlignUp(alias.bytes, kTitlePageBytes));
    }
}

std::optional<std::uint64_t> TitleAddressSpace::AliasBelow4GiB(std::uint64_t start, std::uint64_t bytes, std::string& error) {
    if (bytes == 0) {
        error = "an empty range";
        return std::nullopt;
    }
    if (!Import(start, bytes, error)) return std::nullopt;
    std::lock_guard<std::mutex> guard(lock_);
    if (aliasReservation_ == nullptr) {
        error = "no 1 GiB range below 4 GiB could be reserved for GPU aliases";
        return std::nullopt;
    }
    if (video_) PinSpan(start, bytes);
    const std::uint64_t size = AlignUp(bytes, kTitlePageBytes);
    std::uint64_t slot = aliasBase_;
    for (const auto& [used, alias] : aliases_) {
        if (used - slot >= size) break;
        slot = used + AlignUp(alias.bytes, kTitlePageBytes);
    }
    if (slot + size > aliasBase_ + aliasBytes_) {
        error = "the GPU alias range below 4 GiB is full";
        return std::nullopt;
    }
    aliases_[slot] = Alias{start, bytes, slot};
    PointAliases();
    return slot;
}

void TitleAddressSpace::ReleaseAlias(std::uint64_t alias) {
    std::lock_guard<std::mutex> guard(lock_);
    const auto it = aliases_.find(alias);
    if (it == aliases_.end()) return;
    Clear(it->first, AlignUp(it->second.bytes, kTitlePageBytes));
    aliases_.erase(it);
}

std::uint64_t* TitleAddressSpace::RegionTable(std::uint64_t region, std::string& error) {
    Allocation& table = regions_[region];
    if (table.buffer == VK_NULL_HANDLE) {
        if (!CreateTable(kTitlePagesPerRegion * sizeof(std::uint64_t), table, error)) {
            Destroy(table);
            return nullptr;
        }
        std::fill_n(static_cast<std::uint64_t*>(table.mapped), kTitlePagesPerRegion, nullPage_.address);
        static_cast<std::uint64_t*>(root_.mapped)[region] = table.address;
    }
    return static_cast<std::uint64_t*>(table.mapped);
}

void TitleAddressSpace::Point(std::uint64_t start, std::uint64_t bytes, VkDeviceAddress device, std::uint64_t deviceStart) {
    std::string ignored;
    for (std::uint64_t page = start / kTitlePageBytes; page <= (start + bytes - 1) / kTitlePageBytes; ++page) {
        std::uint64_t* table = RegionTable((page / kTitlePagesPerRegion) % kTitleRegionCount, ignored);
        if (table == nullptr) continue;
        std::uint64_t entry = device + page * kTitlePageBytes - deviceStart;
        if (video_ && !video_->movedTitles.empty()) {
            const auto moved = video_->movedTitles.find(page * kTitlePageBytes);
            if (moved != video_->movedTitles.end()) entry = moved->second;
        }
        table[page % kTitlePagesPerRegion] = entry;
    }
}

void TitleAddressSpace::Clear(std::uint64_t start, std::uint64_t bytes) {
    for (std::uint64_t page = start / kTitlePageBytes; page <= (start + bytes - 1) / kTitlePageBytes; ++page) {
        Allocation& table = regions_[(page / kTitlePagesPerRegion) % kTitleRegionCount];
        if (table.buffer != VK_NULL_HANDLE) static_cast<std::uint64_t*>(table.mapped)[page % kTitlePagesPerRegion] = nullPage_.address;
    }
}

bool TitleAddressSpace::ImportLocked(std::uint64_t start, std::uint64_t bytes, std::string& error) {
    Range range;
    range.bytes = bytes;
    if (!ImportHost(start, bytes, range.imported, error)) return false;
    range.device = range.imported.address;
    range.buffer = range.imported.buffer;
    Point(start, bytes, range.device, start);
    importedBytes_ += bytes;
    ranges_[start] = range;
    if (marking_.load(std::memory_order_relaxed)) AddUseMarks(start, bytes);
    return true;
}

bool TitleAddressSpace::ImportHost(std::uint64_t start, std::uint64_t bytes, Allocation& imported, std::string& error) {
    const DeviceFunctions& vk = device_.Vk();
    VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
    bufferInfo.size = bytes;
    bufferInfo.usage = kImportUsage;
    if (vk.createBuffer(device_.Handle(), &bufferInfo, nullptr, &imported.buffer) != VK_SUCCESS) {
        imported.buffer = VK_NULL_HANDLE;
        error = "vkCreateBuffer failed for imported title memory";
        return false;
    }
    VkMemoryHostPointerPropertiesEXT host{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (device_.HostPointerProperties()(device_.Handle(), VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, reinterpret_cast<void*>(start), &host) != VK_SUCCESS) {
        Destroy(imported);
        error = "vkGetMemoryHostPointerPropertiesEXT refused the title memory";
        return false;
    }
    VkMemoryRequirements requirements{};
    vk.getBufferMemoryRequirements(device_.Handle(), imported.buffer, &requirements);
    const std::optional<std::uint32_t> type = device_.MemoryType(requirements.memoryTypeBits & host.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!type) {
        Destroy(imported);
        error = "no host-coherent memory type can import the title memory";
        return false;
    }
    VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    import.pHostPointer = reinterpret_cast<void*>(start);
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &import};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
    allocate.allocationSize = bytes;
    allocate.memoryTypeIndex = *type;
    const VkResult result = vk.allocateMemory(device_.Handle(), &allocate, nullptr, &imported.memory);
    if (result != VK_SUCCESS) {
        imported.memory = VK_NULL_HANDLE;
        Destroy(imported);
        error = "importing " + std::to_string(bytes >> 20) + " MiB of title memory failed with " + std::to_string(static_cast<int>(result));
        return false;
    }
    if (vk.bindBufferMemory(device_.Handle(), imported.buffer, imported.memory, 0) != VK_SUCCESS) {
        Destroy(imported);
        error = "vkBindBufferMemory failed for imported title memory";
        return false;
    }
    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = imported.buffer;
    imported.address = vk.getBufferDeviceAddress(device_.Handle(), &addressInfo);
    if (imported.address == 0) {
        Destroy(imported);
        error = "imported title memory has no device address";
        return false;
    }
    return true;
}

std::map<std::uint64_t, TitleAddressSpace::Range>::iterator TitleAddressSpace::FirstOverlap(std::uint64_t start) {
    auto it = ranges_.upper_bound(start);
    if (it != ranges_.begin()) {
        auto previous = std::prev(it);
        if (previous->first + previous->second.bytes > start) return previous;
    }
    return it;
}

void TitleAddressSpace::AddBacking(std::uint64_t view, std::uint64_t bytes) {
    if (bytes == 0) return;
    std::lock_guard<std::mutex> guard(lock_);
    backings_[view] = bytes;
}

void TitleAddressSpace::RemoveBacking(std::uint64_t view, std::uint64_t bytes) {
    Doing doing("the backing of physical pages is withdrawn", view);
    std::lock_guard<std::mutex> guard(lock_);
    doing.Stage(kHoldsLock);
    backings_.erase(view);
    if (video_) NoteBackingRemoved(view, bytes);
    const std::uint64_t end = view + bytes;
    auto first = chunks_.lower_bound(view);
    if (first == chunks_.end() || first->first >= end) return;
    {
        const TimedScope timed(TimedPart::AddressIdleWait);
        doing.Stage(kWaitsForQueueLock);
        std::lock_guard<std::mutex> queue(device_.QueueLock());
        doing.Stage(kWaitsForIdleQueue);
        device_.Vk().queueWaitIdle(device_.Queue());
        doing.Stage(kHoldsLock);
    }
    for (auto it = first; it != chunks_.end() && it->first < end;) {
        const VkBuffer buffer = it->second.buffer;
        for (auto range = ranges_.begin(); range != ranges_.end();) {
            if (range->second.imported.memory == VK_NULL_HANDLE && range->second.buffer == buffer) {
                Clear(range->first, range->second.bytes);
                range = ranges_.erase(range);
                continue;
            }
            ++range;
        }
        importedBytes_ -= (std::min)(kBackingChunkBytes, end - it->first);
        Destroy(it->second);
        it = chunks_.erase(it);
    }
    PointAliases();
}

const TitleAddressSpace::Allocation* TitleAddressSpace::BackingChunk(std::uint64_t source, std::uint64_t& chunkStart, std::uint64_t& chunkBytes, std::string& error) {
    auto backing = backings_.upper_bound(source);
    if (backing == backings_.begin()) {
        error = "the source address is in no physical page allocation";
        return nullptr;
    }
    --backing;
    if (source - backing->first >= backing->second) {
        error = "the source address is in no physical page allocation";
        return nullptr;
    }
    chunkStart = backing->first + (source - backing->first) / kBackingChunkBytes * kBackingChunkBytes;
    chunkBytes = (std::min)(kBackingChunkBytes, backing->first + backing->second - chunkStart);
    auto found = chunks_.find(chunkStart);
    if (found != chunks_.end()) return &found->second;
    Allocation imported;
    if (!ImportHost(chunkStart, chunkBytes, imported, error)) return nullptr;
    importedBytes_ += chunkBytes;
    return &chunks_.emplace(chunkStart, imported).first->second;
}

const std::uint8_t* TitleAddressSpace::HostAt(std::uint64_t address) const {
    constexpr std::uint64_t kHostPage = 0x10000;
    Doing doing("AnyBox reads or writes title memory through its physical page", address);
    std::lock_guard<std::mutex> guard(lock_);
    doing.Stage(kHoldsLock);
    const auto found = hostPages_.find(address & ~(kHostPage - 1));
    if (found == hostPages_.end()) {
        if (video_) const_cast<TitleAddressSpace*>(this)->PinOwn(address & ~(kHostPage - 1), kHostPage, PageReturn::ReadByLayer, address);
        return nullptr;
    }
    if (video_) const_cast<TitleAddressSpace*>(this)->PinSource(found->second, PageReturn::ReadByLayer, address);
    return reinterpret_cast<const std::uint8_t*>(found->second + (address & (kHostPage - 1)));
}

bool TitleAddressSpace::AliasPhysical(std::uint64_t start, std::uint64_t bytes, std::uint64_t source, std::string& error, bool gpuMapped) {
    if (bytes == 0) return true;
    Doing doing("a physical page gets a GPU address", start);
    std::lock_guard<std::mutex> guard(lock_);
    doing.Stage(kHoldsLock);
    {
        constexpr std::uint64_t kHostPage = 0x10000;
        if ((start & (kHostPage - 1)) == 0 && (bytes & (kHostPage - 1)) == 0) {
            if (video_) NoteAlias(start, bytes, source, gpuMapped);
            for (std::uint64_t page = 0; page < bytes; page += kHostPage) hostPages_[start + page] = source + page;
        }
    }
    const std::uint64_t end = start + bytes;
    for (auto it = FirstOverlap(start); it != ranges_.end() && it->first < end;) {
        if (it->second.imported.memory != VK_NULL_HANDLE) {
            error = "the aliased range overlaps title memory imported on its own";
            return false;
        }
        const Range kept = it->second;
        const std::uint64_t keptStart = it->first;
        const std::uint64_t keptEnd = keptStart + kept.bytes;
        Clear(keptStart, kept.bytes);
        it = ranges_.erase(it);
        if (keptStart < start) {
            Point(keptStart, start - keptStart, kept.device, keptStart);
            ranges_[keptStart] = Range{start - keptStart, kept.device, {}, kept.buffer, kept.bufferOffset};
        }
        if (keptEnd > end) {
            Point(end, keptEnd - end, kept.device + (end - keptStart), end);
            ranges_[end] = Range{keptEnd - end, kept.device + (end - keptStart), {}, kept.buffer, kept.bufferOffset + (end - keptStart)};
            break;
        }
    }
    std::uint64_t at = start;
    std::uint64_t from = source;
    bool complete = true;
    while (at < end) {
        std::uint64_t chunkStart = 0;
        std::uint64_t chunkBytes = 0;
        const Allocation* chunk = BackingChunk(from, chunkStart, chunkBytes, error);
        if (chunk == nullptr) {
            Clear(at, end - at);
            complete = false;
            break;
        }
        const std::uint64_t piece = (std::min)(end - at, chunkStart + chunkBytes - from);
        const VkDeviceAddress device = chunk->address + (from - chunkStart);
        Point(at, piece, device, at);
        ranges_[at] = Range{piece, device, {}, chunk->buffer, from - chunkStart};
        at += piece;
        from += piece;
    }
    PointAliases();
    return complete;
}

bool TitleAddressSpace::Import(std::uint64_t start, std::uint64_t bytes, std::string& error) {
    if (bytes == 0) return true;
    const std::uint64_t alignment = (std::max<std::uint64_t>)(device_.HostImportAlignment(), 4096);
    std::uint64_t first = AlignDown(start, alignment);
    std::uint64_t last = AlignUp(start + bytes, alignment);
    Doing doing("title memory committed on its own is imported", start);
    std::lock_guard<std::mutex> guard(lock_);
    doing.Stage(kHoldsLock);
    if (video_) {
        const auto moved = video_->moved.lower_bound(AlignDown(first, kTitlePageBytes));
        if (moved != video_->moved.end() && moved->first < last && moved->second.own) PinOwn(first, last - first, PageReturn::Remapped, start);
    }
    {
        auto containing = ranges_.upper_bound(first);
        if (containing != ranges_.begin()) {
            --containing;
            if (containing->first + containing->second.bytes >= last) return true;
        }
    }
    bool overlapped = false;
    for (auto it = FirstOverlap(first); it != ranges_.end() && it->first < last;) {
        const std::uint64_t rangeEnd = it->first + it->second.bytes;
        if (it->first < last && rangeEnd > first && it->second.imported.memory != VK_NULL_HANDLE) {
            if (video_) ReturnOwnSpan(it->first, rangeEnd, PageReturn::Remapped);
            if (!overlapped) {
                const TimedScope timed(TimedPart::AddressIdleWait);
                doing.Stage(kWaitsForQueueLock);
                std::lock_guard<std::mutex> queue(device_.QueueLock());
                doing.Stage(kWaitsForIdleQueue);
                device_.Vk().queueWaitIdle(device_.Queue());
                doing.Stage(kHoldsLock);
                overlapped = true;
            }
            first = (std::min)(first, it->first);
            last = (std::max)(last, rangeEnd);
            importedBytes_ -= it->second.bytes;
            Destroy(it->second.imported);
            it = ranges_.erase(it);
            continue;
        }
        ++it;
    }
    const bool imported = ImportLocked(first, last - first, error);
    PointAliases();
    return imported;
}

bool TitleAddressSpace::MapDevice(std::uint64_t start, std::uint64_t bytes, VkDeviceAddress device, VkBuffer buffer, std::string& error) {
    if (bytes == 0 || device == 0) {
        error = "an empty range or no device address";
        return false;
    }
    std::lock_guard<std::mutex> guard(lock_);
    Range range;
    range.bytes = bytes;
    range.device = device;
    range.buffer = buffer;
    Point(start, bytes, device, start);
    ranges_[start] = range;
    return true;
}

void TitleAddressSpace::Release(std::uint64_t start, std::uint64_t bytes) {
    if (bytes == 0) return;
    const std::uint64_t end = start + bytes;
    Doing doing("a GPU address range is released", start);
    std::lock_guard<std::mutex> guard(lock_);
    doing.Stage(kHoldsLock);
    if (video_) NoteRelease(start, end);
    hostPages_.erase(hostPages_.lower_bound(start & ~std::uint64_t{0xFFFF}), hostPages_.lower_bound(end));
    std::vector<std::pair<std::uint64_t, Range>> released;
    for (auto it = FirstOverlap(start); it != ranges_.end() && it->first < end;) {
        if (it->first + it->second.bytes > start) {
            released.emplace_back(it->first, it->second);
            it = ranges_.erase(it);
            continue;
        }
        ++it;
    }
    if (released.empty()) return;
    bool imported = false;
    for (const auto& [rangeStart, range] : released) imported = imported || range.imported.memory != VK_NULL_HANDLE;
    if (imported) {
        const TimedScope timed(TimedPart::AddressIdleWait);
        doing.Stage(kWaitsForQueueLock);
        std::lock_guard<std::mutex> queue(device_.QueueLock());
        doing.Stage(kWaitsForIdleQueue);
        device_.Vk().queueWaitIdle(device_.Queue());
        doing.Stage(kHoldsLock);
    }
    std::string ignored;
    for (auto& [rangeStart, range] : released) {
        Clear(rangeStart, range.bytes);
        const std::uint64_t rangeEnd = rangeStart + range.bytes;
        if (range.imported.memory == VK_NULL_HANDLE) {
            if (rangeStart < start) {
                Point(rangeStart, start - rangeStart, range.device, rangeStart);
                ranges_[rangeStart] = Range{start - rangeStart, range.device, {}, range.buffer, range.bufferOffset};
            }
            if (rangeEnd > end) {
                Point(end, rangeEnd - end, range.device + (end - rangeStart), end);
                ranges_[end] = Range{rangeEnd - end, range.device + (end - rangeStart), {}, range.buffer, range.bufferOffset + (end - rangeStart)};
            }
            continue;
        }
        importedBytes_ -= range.bytes;
        Destroy(range.imported);
        if (rangeStart < start) ImportLocked(rangeStart, start - rangeStart, ignored);
        if (rangeEnd > end) ImportLocked(end, rangeEnd - end, ignored);
    }
    for (auto it = FirstOverlap(AlignDown(start, kTitlePageBytes)); it != ranges_.end() && it->first < AlignUp(end, kTitlePageBytes); ++it) {
        Point(it->first, it->second.bytes, it->second.device, it->first);
    }
    PointAliases();
}

bool TitleAddressSpace::Contains(std::uint64_t address, std::uint64_t bytes) const {
    std::lock_guard<std::mutex> guard(lock_);
    auto it = ranges_.upper_bound(address);
    if (it == ranges_.begin()) return false;
    --it;
    return address - it->first <= it->second.bytes && bytes <= it->second.bytes - (address - it->first);
}

std::optional<TitleAddressSpace::Location> TitleAddressSpace::Locate(std::uint64_t address, std::uint64_t bytes) const {
    Doing doing("a command names title memory as a buffer", address);
    std::lock_guard<std::mutex> guard(lock_);
    doing.Stage(kHoldsLock);
    TitleAddressSpace& self = const_cast<TitleAddressSpace&>(*this);
    auto it = self.ranges_.upper_bound(address);
    if (it == self.ranges_.begin()) return std::nullopt;
    --it;
    const std::uint64_t offset = address - it->first;
    if (offset > it->second.bytes || bytes > it->second.bytes - offset || it->second.buffer == VK_NULL_HANDLE) return std::nullopt;
    if (video_) self.PinRange(it, address, bytes);
    return Location{it->second.buffer, it->second.bufferOffset + offset};
}

std::optional<TitleAddressSpace::Location> TitleAddressSpace::LocateRun(std::uint64_t address, std::uint64_t& available, std::uint64_t wanted) const {
    Doing doing("a command names title memory as a buffer", address);
    std::lock_guard<std::mutex> guard(lock_);
    doing.Stage(kHoldsLock);
    available = 0;
    TitleAddressSpace& self = const_cast<TitleAddressSpace&>(*this);
    auto it = self.ranges_.upper_bound(address);
    if (it == self.ranges_.begin()) return std::nullopt;
    --it;
    const std::uint64_t offset = address - it->first;
    if (offset >= it->second.bytes || it->second.buffer == VK_NULL_HANDLE) return std::nullopt;
    if (video_) self.PinRange(it, address, (std::min)(wanted, it->second.bytes - offset));
    available = it->second.bytes - offset;
    return Location{it->second.buffer, it->second.bufferOffset + offset};
}

std::string TitleAddressSpace::DescribeTitleAddress(std::uint64_t address) const {
    std::lock_guard<std::mutex> guard(lock_);
    auto it = ranges_.upper_bound(address);
    char text[160];
    if (it != ranges_.begin()) {
        --it;
        std::snprintf(text, sizeof(text), "nearest range below starts 0x%llx, 0x%llx bytes, %s", static_cast<unsigned long long>(it->first),
                      static_cast<unsigned long long>(it->second.bytes), it->second.imported.memory != VK_NULL_HANDLE ? "imported" : "mapped");
        return text;
    }
    return "no range below";
}

std::string TitleAddressSpace::DescribeDeviceAddress(std::uint64_t device) const {
    std::lock_guard<std::mutex> guard(lock_);
    std::string best = "in no title address range";
    std::uint64_t bestDistance = UINT64_MAX;
    bool inside = false;
    const auto consider = [&](const char* kind, std::uint64_t title, std::uint64_t start, std::uint64_t bytes) {
        if (start == 0 || inside) return;
        char text[192];
        if (device >= start && device - start < bytes) {
            std::snprintf(text, sizeof(text), "inside %s for title 0x%llx (+0x%llx of 0x%llx bytes)", kind, static_cast<unsigned long long>(title),
                          static_cast<unsigned long long>(device - start), static_cast<unsigned long long>(bytes));
            best = text;
            bestDistance = 0;
            inside = true;
            return;
        }
        if (device >= start + bytes && device - (start + bytes) < bestDistance) {
            bestDistance = device - (start + bytes);
            std::snprintf(text, sizeof(text), "0x%llx past the end of %s for title 0x%llx (0x%llx bytes)", static_cast<unsigned long long>(bestDistance), kind,
                          static_cast<unsigned long long>(title), static_cast<unsigned long long>(bytes));
            best = text;
        }
    };
    for (const auto& [start, range] : ranges_) {
        if (inside) break;
        consider(range.imported.memory != VK_NULL_HANDLE ? "imported title memory" : "a mapped range", start, range.device, range.bytes);
    }
    for (const auto& [view, chunk] : chunks_) {
        if (inside) break;
        consider("a physical page chunk", view, chunk.address, kBackingChunkBytes);
    }
    if (video_) {
        for (const VideoBlock& block : video_->blocks) {
            if (inside) break;
            consider("a block of title pages kept in video memory", 0, block.allocation.address, block.bytes);
        }
    }
    consider("the null page", 0, nullPage_.address, 2 * kTitlePageBytes);
    consider("the null region table", 0, nullRegion_.address, kTitlePagesPerRegion * sizeof(std::uint64_t));
    for (std::size_t region = 0; region < regions_.size() && !inside; ++region) {
        if (regions_[region].buffer != VK_NULL_HANDLE) consider("a region page table", region << 32, regions_[region].address, kTitlePagesPerRegion * sizeof(std::uint64_t));
    }
    consider("the page table root", 0, root_.address, kTitleRegionCount * sizeof(std::uint64_t));
    return best;
}

std::uint64_t TitleAddressSpace::ReadForTrace(std::uint64_t address, void* destination, std::uint64_t bytes) const {
    std::uint64_t done = 0;
    while (done < bytes) {
        const std::uint64_t at = address + done;
        const std::uint64_t page = AlignDown(at, kTitlePageBytes);
        const std::uint64_t run = (std::min)(bytes - done, page + kTitlePageBytes - at);
        std::lock_guard<std::mutex> guard(lock_);
        const auto host = hostPages_.find(page);
        const std::uint8_t* from = nullptr;
        if (video_) {
            const auto moved = video_->moved.find(host != hostPages_.end() ? host->second : page);
            if (moved != video_->moved.end() && (host != hostPages_.end() || moved->second.own)) {
                from = static_cast<const std::uint8_t*>(video_->blocks[moved->second.block].allocation.mapped) + std::uint64_t{moved->second.page} * kTitlePageBytes + (at - page);
            }
        }
        if (from == nullptr && host != hostPages_.end()) from = reinterpret_cast<const std::uint8_t*>(host->second + (at - page));
        if (from == nullptr) {
            MEMORY_BASIC_INFORMATION region{};
            if (VirtualQuery(reinterpret_cast<const void*>(at), &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT ||
                (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 || reinterpret_cast<std::uint64_t>(region.BaseAddress) + region.RegionSize < at + run) {
                break;
            }
            from = reinterpret_cast<const std::uint8_t*>(at);
        }
        std::memcpy(static_cast<std::uint8_t*>(destination) + done, from, static_cast<std::size_t>(run));
        done += run;
    }
    return done;
}

std::string TitleAddressSpace::DescribeBacking(std::uint64_t address, std::uint64_t bytes) const {
    constexpr std::uint64_t kSamples = 16;
    if (bytes == 0 || address + bytes < address) return "no pages";
    std::lock_guard<std::mutex> guard(lock_);
    const std::uint64_t first = address / kTitlePageBytes;
    const std::uint64_t pages = (address + bytes - 1) / kTitlePageBytes - first + 1;
    const std::uint64_t step = (pages + kSamples - 1) / kSamples;
    std::uint64_t sampled = 0, video = 0, imported = 0, chunk = 0, other = 0;
    std::string firstText;
    for (std::uint64_t page = 0; page < pages; page += step) {
        const std::uint64_t title = (first + page) * kTitlePageBytes;
        const std::uint64_t probe = (std::max)(title, address);
        char where[192];
        auto range = ranges_.upper_bound(probe);
        const bool mapped = range != ranges_.begin() && probe - std::prev(range)->first < std::prev(range)->second.bytes;
        if (mapped) --range;
        if (video_ && video_->movedTitles.count(title) != 0) {
            ++video;
            std::snprintf(where, sizeof(where), "inside a block of title pages kept in video memory");
        } else if (!mapped) {
            ++other;
            std::snprintf(where, sizeof(where), "in no title address range");
        } else {
            const bool own = range->second.imported.memory != VK_NULL_HANDLE;
            const bool physical = !own && hostPages_.count(title) != 0;
            ++(own ? imported : physical ? chunk : other);
            std::snprintf(where, sizeof(where), "inside %s for title 0x%llx (+0x%llx of 0x%llx bytes)",
                          own ? "imported title memory" : physical ? "a physical page chunk" : "a mapped range", static_cast<unsigned long long>(range->first),
                          static_cast<unsigned long long>(probe - range->first), static_cast<unsigned long long>(range->second.bytes));
        }
        ++sampled;
        if (firstText.empty()) firstText = where;
    }
    char text[256];
    std::snprintf(text, sizeof(text), "%llu of its %llu pages looked at: %llu in video memory blocks, %llu in imported title memory, %llu in physical page chunks, %llu elsewhere",
                  static_cast<unsigned long long>(sampled), static_cast<unsigned long long>(pages), static_cast<unsigned long long>(video), static_cast<unsigned long long>(imported),
                  static_cast<unsigned long long>(chunk), static_cast<unsigned long long>(other));
    return std::string(text) + " (first: " + firstText + ")";
}

std::string TitleAddressSpace::DescribePlacement(std::uint64_t address, std::uint64_t bytes) const {
    constexpr std::uint64_t kMostPages = 4096;
    constexpr std::size_t kMostBuffers = 64;
    constexpr std::size_t kBuffersNamed = 2;
    std::lock_guard<std::mutex> guard(lock_);
    if (!video_ || bytes == 0 || address + bytes < address) return "title pages: not watched";
    const VideoPages& video = *video_;
    const std::uint64_t end = address + bytes;
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
    std::uint64_t overlapping = 0;
    std::string named;
    for (auto it = video.buffers.begin(); it != video.buffers.end() && it->first < end; ++it) {
        if (it->first + it->second.bytes <= address) continue;
        ++overlapping;
        if (spans.size() < kMostBuffers) spans.emplace_back(it->first, it->first + it->second.bytes);
        if (overlapping > kBuffersNamed) continue;
        const char* state = it->second.home       ? "back in system memory for good"
                            : it->second.complete ? "decided"
                            : now - it->second.added < std::chrono::seconds(video.setup.settleSeconds) ? "settling"
                                                                                                     : "open";
        char text[96];
        std::snprintf(text, sizeof(text), "%s0x%llx %llu B %s", named.empty() ? "" : ", ", static_cast<unsigned long long>(it->first),
                      static_cast<unsigned long long>(it->second.bytes), state);
        named += text;
    }
    const std::uint64_t first = AlignDown(address, kTitlePageBytes);
    const std::uint64_t pages = (std::min)((AlignUp(end, kTitlePageBytes) - first) / kTitlePageBytes, kMostPages);
    std::uint64_t moved = 0, copies = 0, whole = 0, partly = 0, outside = 0, pinned = 0, shared = 0, most = 0, none = 0;
    for (std::uint64_t index = 0; index < pages; ++index) {
        const std::uint64_t title = first + index * kTitlePageBytes;
        bool inWhole = false;
        bool inPart = false;
        for (const auto& [from, to] : spans) {
            if (from <= title && title + kTitlePageBytes <= to) inWhole = true;
            if (from < title + kTitlePageBytes && to > title) inPart = true;
        }
        whole += inWhole ? 1 : 0;
        partly += !inWhole && inPart ? 1 : 0;
        outside += inPart ? 0 : 1;
        const auto host = hostPages_.find(title);
        const std::uint64_t source = host != hostPages_.end() ? host->second : title;
        const auto found = video.moved.find(source);
        if (found != video.moved.end() && (host != hostPages_.end() || found->second.own)) {
            ++(found->second.readOnly ? copies : moved);
            continue;
        }
        if (host == hostPages_.end()) {
            if (!OwnRange(title)) {
                ++none;
            } else if (OwnPinned(title)) {
                ++pinned;
            }
            continue;
        }
        if (video.pinned.count(source) != 0) ++pinned;
        const std::uint64_t titles = video.titlesOf.count(source);
        most = (std::max)(most, titles);
        if (titles > kVideoPageTitles) ++shared;
    }
    char text[512];
    std::snprintf(text, sizeof(text),
                  "title pages: %llu watched buffer(s) the GPU may write overlap it%s%s%s; of %llu page(s): %llu in video memory, %llu read-only copies, %llu wholly under "
                  "such a buffer, %llu only partly, %llu under none, %llu pinned by an earlier use, %llu physical with more than %u GPU addresses (most %llu), %llu neither "
                  "physical nor committed on its own",
                  static_cast<unsigned long long>(overlapping), named.empty() ? "" : " (", named.c_str(), named.empty() ? "" : (overlapping > kBuffersNamed ? ", ...)" : ")"),
                  static_cast<unsigned long long>(pages), static_cast<unsigned long long>(moved), static_cast<unsigned long long>(copies),
                  static_cast<unsigned long long>(whole), static_cast<unsigned long long>(partly), static_cast<unsigned long long>(outside),
                  static_cast<unsigned long long>(pinned), static_cast<unsigned long long>(shared), kVideoPageTitles, static_cast<unsigned long long>(most),
                  static_cast<unsigned long long>(none));
    return text;
}

std::uint64_t TitleAddressSpace::ImportedBytes() const {
    std::lock_guard<std::mutex> guard(lock_);
    return importedBytes_;
}

bool TitleAddressSpace::CreateVideoBlock(std::uint64_t bytes, Allocation& block, std::string& error) {
    const DeviceFunctions& vk = device_.Vk();
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = bytes;
    bufferInfo.usage = kTableUsage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vk.createBuffer(device_.Handle(), &bufferInfo, nullptr, &block.buffer) != VK_SUCCESS) {
        block.buffer = VK_NULL_HANDLE;
        error = "vkCreateBuffer failed for a block of title pages in video memory";
        return false;
    }
    VkMemoryRequirements requirements{};
    vk.getBufferMemoryRequirements(device_.Handle(), block.buffer, &requirements);
    const std::optional<std::uint32_t> type =
        device_.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!type) {
        error = "the device has no video memory type the CPU can map, so title pages cannot be copied into video memory and back";
        return false;
    }
    VkMemoryPriorityAllocateInfoEXT priority{VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT};
    priority.priority = 1.0f;
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    if (device_.PageableMemory()) flags.pNext = &priority;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = *type;
    if (vk.allocateMemory(device_.Handle(), &allocate, nullptr, &block.memory) != VK_SUCCESS) {
        block.memory = VK_NULL_HANDLE;
        error = "vkAllocateMemory failed for a block of title pages in video memory";
        return false;
    }
    if (vk.bindBufferMemory(device_.Handle(), block.buffer, block.memory, 0) != VK_SUCCESS ||
        vk.mapMemory(device_.Handle(), block.memory, 0, VK_WHOLE_SIZE, 0, &block.mapped) != VK_SUCCESS) {
        error = "a block of title pages in video memory could not be bound or mapped";
        return false;
    }
    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = block.buffer;
    block.address = vk.getBufferDeviceAddress(device_.Handle(), &addressInfo);
    if (block.address == 0) {
        error = "a block of title pages in video memory has no device address";
        return false;
    }
    return true;
}

bool TitleAddressSpace::EnableVideoPages(const VideoPageSetup& setup, std::string& error) {
    if (setup.capBytes + setup.readCapBytes + setup.readOwnCapBytes < kTitlePageBytes || setup.guard == nullptr) {
        error = "no room for one page or no way to take the CPU's access to a page away";
        return false;
    }
    std::lock_guard<std::mutex> guard(lock_);
    if (video_) {
        error = "title pages in video memory are already on";
        return false;
    }
    auto video = std::make_unique<VideoPages>();
    video->setup = setup;
    video->setup.capBytes = AlignDown(setup.capBytes, kTitlePageBytes);
    video->setup.readCapBytes = AlignDown(setup.readCapBytes, kTitlePageBytes);
    video->setup.readOwnCapBytes = AlignDown(setup.readOwnCapBytes, kTitlePageBytes);
    video->arenaCap = video->setup.capBytes + video->setup.readCapBytes + video->setup.readOwnCapBytes;
    VideoBlock first;
    first.bytes = (std::min)(kVideoBlockBytes, video->arenaCap);
    if (!CreateVideoBlock(first.bytes, first.allocation, error)) {
        Destroy(first.allocation);
        return false;
    }
    for (std::uint64_t page = first.bytes / kTitlePageBytes; page != 0; --page) first.free.push_back(static_cast<std::uint32_t>(page - 1));
    video->arenaBytes = first.bytes;
    video->blocks.push_back(std::move(first));
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = device_.QueueFamily();
    VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    if (device_.Vk().createCommandPool(device_.Handle(), &poolInfo, nullptr, &video->pool) == VK_SUCCESS) {
        allocateInfo.commandPool = video->pool;
        if (device_.Vk().allocateCommandBuffers(device_.Handle(), &allocateInfo, &video->commands) != VK_SUCCESS) video->commands = VK_NULL_HANDLE;
    } else {
        video->pool = VK_NULL_HANDLE;
    }
    if (video->commands == VK_NULL_HANDLE) {
        error = "no command buffer for copying title pages back out of video memory";
        if (video->pool != VK_NULL_HANDLE) device_.Vk().destroyCommandPool(device_.Handle(), video->pool, nullptr);
        for (VideoBlock& block : video->blocks) Destroy(block.allocation);
        return false;
    }
    for (const auto& [title, source] : hostPages_) video->titlesOf.emplace(source, title);
    video_ = std::move(video);
    tableCommands_.store(video_->setup.tableCommands && video_->setup.capBytes != 0, std::memory_order_relaxed);
    static const bool handled = AddVectoredExceptionHandler(1, &OnGuardedPage) != nullptr;
    if (!handled) {
        error = "the access handler for guarded title pages could not be installed";
        device_.Vk().destroyCommandPool(device_.Handle(), video_->pool, nullptr);
        for (VideoBlock& block : video_->blocks) Destroy(block.allocation);
        video_.reset();
        tableCommands_.store(false, std::memory_order_relaxed);
        return false;
    }
    g_guarded.store(this, std::memory_order_release);
    if (video_->setup.readOwnCapBytes != 0) {
        if (device_.Vk().createCommandPool(device_.Handle(), &poolInfo, nullptr, &video_->usePool) == VK_SUCCESS) {
            allocateInfo.commandPool = video_->usePool;
            if (device_.Vk().allocateCommandBuffers(device_.Handle(), &allocateInfo, &video_->useCommands) != VK_SUCCESS) video_->useCommands = VK_NULL_HANDLE;
        } else {
            video_->usePool = VK_NULL_HANDLE;
        }
        useMarks_.reset(new std::atomic<UseMarks*>[kTitleRegionCount]());
        epochMilliseconds_.store((std::max)(video_->setup.useMarkEpochMilliseconds, std::uint32_t{1}), std::memory_order_relaxed);
        marking_.store(true, std::memory_order_release);
        for (const auto& [start, range] : ranges_) {
            if (range.imported.memory != VK_NULL_HANDLE) AddUseMarks(start, range.bytes);
        }
    }
    return true;
}

void TitleAddressSpace::AddUseMarks(std::uint64_t start, std::uint64_t bytes) {
    if (!video_ || !useMarks_ || bytes == 0 || start >= kTitleAddressLimit) return;
    const std::uint64_t last = (std::min)(start + bytes, kTitleAddressLimit) - 1;
    for (std::uint64_t region = start / kTitleRegionBytes; region <= last / kTitleRegionBytes; ++region) {
        if (useMarks_[region].load(std::memory_order_relaxed) != nullptr) continue;
        std::unique_ptr<UseMarks[]> marks(new UseMarks[kTitlePagesPerRegion]());
        useMarks_[region].store(marks.get(), std::memory_order_release);
        video_->markRegions.push_back(region);
        video_->markArrays.push_back(std::move(marks));
    }
}

TitleAddressSpace::UseMarks* TitleAddressSpace::MarkAt(std::uint64_t title) const {
    if (!useMarks_ || title >= kTitleAddressLimit) return nullptr;
    UseMarks* const marks = useMarks_[title / kTitleRegionBytes].load(std::memory_order_acquire);
    return marks != nullptr ? marks + (title % kTitleRegionBytes) / kTitlePageBytes : nullptr;
}

std::uint8_t TitleAddressSpace::UseEpoch() const {
    return static_cast<std::uint8_t>(GetTickCount64() / epochMilliseconds_.load(std::memory_order_relaxed) % kMarkEpochs + 1);
}

void TitleAddressSpace::SetHeldMark(std::uint64_t title, bool held) {
    UseMarks* const mark = MarkAt(title);
    if (mark == nullptr) return;
    if (held) {
        mark->fetch_or(kMarkHeld, std::memory_order_relaxed);
    } else {
        mark->fetch_and(static_cast<std::uint8_t>(~kMarkHeld), std::memory_order_relaxed);
    }
}

void TitleAddressSpace::MarkGpuRead(std::uint64_t address, std::uint64_t bytes) {
    if (!marking_.load(std::memory_order_acquire) || bytes == 0 || address >= kTitleAddressLimit) return;
    const std::uint8_t epoch = UseEpoch();
    const std::uint64_t last = (std::min)(address + (std::min)(bytes, kMarkSpanBytes), kTitleAddressLimit) - 1;
    for (std::uint64_t page = address / kTitlePageBytes; page <= last / kTitlePageBytes; ++page) {
        UseMarks* const marks = useMarks_[page / kTitlePagesPerRegion].load(std::memory_order_acquire);
        if (marks == nullptr) {
            page = (page / kTitlePagesPerRegion + 1) * kTitlePagesPerRegion - 1;
            continue;
        }
        UseMarks& mark = marks[page % kTitlePagesPerRegion];
        std::uint8_t old = mark.load(std::memory_order_relaxed);
        while ((old & kMarkEpochs) != epoch && !mark.compare_exchange_weak(old, static_cast<std::uint8_t>((old & ~kMarkEpochs) | epoch), std::memory_order_relaxed)) {
        }
    }
}

void TitleAddressSpace::MarkGpuRootWords(const std::uint32_t* words, std::uint32_t count) {
    if (!marking_.load(std::memory_order_acquire) || words == nullptr) return;
    for (std::uint32_t index = 0; index + 1 < count; ++index) {
        const std::uint32_t high = words[index + 1] & 0xFFFFu;
        if (high == 0) continue;
        const std::uint64_t address = words[index] | (static_cast<std::uint64_t>(high) << 32);
        if (address >= kTitleAddressLimit || useMarks_[address / kTitleRegionBytes].load(std::memory_order_relaxed) == nullptr) continue;
        MarkGpuRead(AlignDown(address, kTitlePageBytes), kRootWordMarkBytes);
    }
}

void TitleAddressSpace::MarkGpuRootWrite(std::uint64_t address) {
    if (!marking_.load(std::memory_order_acquire) || address >= kTitleAddressLimit) return;
    bool fresh = false;
    const std::uint64_t first = AlignDown(address, kTitlePageBytes);
    for (std::uint64_t title = first; title < first + kRootWriteMarkBytes; title += kTitlePageBytes) {
        UseMarks* const mark = MarkAt(title);
        if (mark != nullptr) fresh = (mark->fetch_or(kMarkRootWrites, std::memory_order_relaxed) & kMarkRootWrites) == 0 || fresh;
    }
    if (fresh) MarkGpuWritten(first, kRootWriteMarkBytes);
}

bool TitleAddressSpace::TakeVideoPage(std::uint32_t& block, std::uint32_t& page) {
    VideoPages& video = *video_;
    for (std::size_t index = video.blocks.size(); index != 0; --index) {
        VideoBlock& candidate = video.blocks[index - 1];
        if (candidate.free.empty()) continue;
        block = static_cast<std::uint32_t>(index - 1);
        page = candidate.free.back();
        candidate.free.pop_back();
        return true;
    }
    if (video.arenaBytes >= video.arenaCap) return false;
    VideoBlock added;
    added.bytes = (std::min)(kVideoBlockBytes, video.arenaCap - video.arenaBytes);
    std::string error;
    if (!CreateVideoBlock(added.bytes, added.allocation, error)) {
        Destroy(added.allocation);
        video.arenaCap = video.arenaBytes;
        return false;
    }
    for (std::uint64_t index = added.bytes / kTitlePageBytes; index != 0; --index) added.free.push_back(static_cast<std::uint32_t>(index - 1));
    video.arenaBytes += added.bytes;
    video.blocks.push_back(std::move(added));
    block = static_cast<std::uint32_t>(video.blocks.size() - 1);
    page = video.blocks.back().free.back();
    video.blocks.back().free.pop_back();
    return true;
}

bool TitleAddressSpace::WaitForQueue(std::uint64_t timeoutNanoseconds) {
    const DeviceFunctions& vk = device_.Vk();
    std::vector<VkFence>& late = video_->lateFences;
    for (std::size_t index = 0; index < late.size();) {
        if (vk.getFenceStatus(device_.Handle(), late[index]) != VK_SUCCESS) {
            ++index;
            continue;
        }
        vk.destroyFence(device_.Handle(), late[index], nullptr);
        late[index] = late.back();
        late.pop_back();
    }
    VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (vk.createFence(device_.Handle(), &info, nullptr, &fence) != VK_SUCCESS) return false;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (vk.queueSubmit(device_.Queue(), 1, &submit, fence) != VK_SUCCESS) {
        vk.destroyFence(device_.Handle(), fence, nullptr);
        return false;
    }
    if (vk.waitForFences(device_.Handle(), 1, &fence, VK_TRUE, timeoutNanoseconds) != VK_SUCCESS) {
        late.push_back(fence);
        return false;
    }
    vk.destroyFence(device_.Handle(), fence, nullptr);
    return true;
}

void TitleAddressSpace::SetEntry(std::uint64_t title, std::uint64_t entry) {
    std::string ignored;
    const std::uint64_t page = title / kTitlePageBytes;
    std::uint64_t* table = RegionTable((page / kTitlePagesPerRegion) % kTitleRegionCount, ignored);
    if (table != nullptr) table[page % kTitlePagesPerRegion] = entry;
}

std::uint64_t TitleAddressSpace::HostEntry(std::uint64_t title) const {
    auto it = ranges_.upper_bound(title);
    if (it == ranges_.begin()) return nullPage_.address;
    --it;
    if (title - it->first >= it->second.bytes) return nullPage_.address;
    return it->second.device + (title - it->first);
}

std::uint64_t TitleAddressSpace::TableEntry(std::uint64_t address) const {
    std::lock_guard<std::mutex> guard(lock_);
    const std::uint64_t page = address / kTitlePageBytes;
    const Allocation& table = regions_[(page / kTitlePagesPerRegion) % kTitleRegionCount];
    if (table.buffer == VK_NULL_HANDLE) return nullPage_.address;
    return static_cast<const std::uint64_t*>(table.mapped)[page % kTitlePagesPerRegion];
}

void TitleAddressSpace::AddEvent(const VideoPageEvent& event) {
    VideoPageReport& report = video_->report;
    if (report.events.size() >= kVideoPageEvents) {
        ++report.droppedEvents;
        return;
    }
    report.events.push_back(event);
}

void TitleAddressSpace::ForgetTitle(std::uint64_t source, std::uint64_t title) {
    const auto [first, last] = video_->titlesOf.equal_range(source);
    for (auto it = first; it != last; ++it) {
        if (it->second == title) {
            video_->titlesOf.erase(it);
            return;
        }
    }
}

bool TitleAddressSpace::CopyPagesOut(const std::vector<std::uint64_t>& sources) {
    VideoPages& video = *video_;
    const DeviceFunctions& vk = device_.Vk();
    std::map<std::pair<VkBuffer, VkBuffer>, std::vector<VkBufferCopy>> copies;
    for (const std::uint64_t source : sources) {
        const auto found = video.moved.find(source);
        if (found == video.moved.end()) continue;
        const MovedPage& page = found->second;
        auto range = ranges_.upper_bound(page.title);
        if (range == ranges_.begin()) return false;
        --range;
        const std::uint64_t within = page.title - range->first;
        if (within >= range->second.bytes || range->second.bytes - within < kTitlePageBytes || range->second.buffer == VK_NULL_HANDLE) return false;
        copies[{video.blocks[page.block].allocation.buffer, range->second.buffer}].push_back(
            VkBufferCopy{std::uint64_t{page.page} * kTitlePageBytes, range->second.bufferOffset + within, kTitlePageBytes});
    }
    if (copies.empty()) return true;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vk.resetCommandPool(device_.Handle(), video.pool, 0) != VK_SUCCESS || vk.beginCommandBuffer(video.commands, &begin) != VK_SUCCESS) return false;
    for (const auto& [buffers, regions] : copies) {
        vk.cmdCopyBuffer(video.commands, buffers.first, buffers.second, static_cast<std::uint32_t>(regions.size()), regions.data());
    }
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (vk.endCommandBuffer(video.commands) != VK_SUCCESS || vk.createFence(device_.Handle(), &fenceInfo, nullptr, &fence) != VK_SUCCESS) return false;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &video.commands;
    if (vk.queueSubmit(device_.Queue(), 1, &submit, fence) != VK_SUCCESS) {
        vk.destroyFence(device_.Handle(), fence, nullptr);
        return false;
    }
    StageOfThisThread(kWaitsForCopy);
    VkResult waited = VK_TIMEOUT;
    while (waited == VK_TIMEOUT) waited = vk.waitForFences(device_.Handle(), 1, &fence, VK_TRUE, kReturnWaitNanoseconds);
    vk.destroyFence(device_.Handle(), fence, nullptr);
    return waited == VK_SUCCESS;
}

void TitleAddressSpace::ReturnPagesHeld(const std::vector<std::uint64_t>& sources, PageReturn reason, bool idle) {
    VideoPages& video = *video_;
    const auto begin = std::chrono::steady_clock::now();
    bool own = false;
    for (const std::uint64_t source : sources) {
        const auto found = video.moved.find(source);
        own = own || (found != video.moved.end() && found->second.own);
    }
    const bool copied = idle && (own || sources.size() >= kGpuCopyPages) && CopyPagesOut(sources);
    for (const std::uint64_t source : sources) {
        const auto found = video.moved.find(source);
        if (found == video.moved.end()) continue;
        const MovedPage page = found->second;
        VideoBlock& block = video.blocks[page.block];
        const std::uint8_t* const held = static_cast<const std::uint8_t*>(block.allocation.mapped) + std::uint64_t{page.page} * kTitlePageBytes;
        DWORD previous = 0;
        if (!copied && page.own) {
            video.staging.resize(kTitlePageBytes);
            std::memcpy(video.staging.data(), held, kTitlePageBytes);
            if (VirtualProtect(reinterpret_cast<void*>(source), kTitlePageBytes, PAGE_READWRITE, &previous)) {
                std::memcpy(reinterpret_cast<void*>(source), video.staging.data(), kTitlePageBytes);
            } else {
                ++video.report.lostPages;
            }
            ++video.report.slowCopyPages;
        } else if (!copied) {
            std::memcpy(reinterpret_cast<void*>(source), held, kTitlePageBytes);
            ++video.report.slowCopyPages;
        }
        video.movedTitles.erase(page.title);
        for (std::uint32_t index = 0; index < page.extraCount; ++index) video.movedTitles.erase(page.extra[index]);
        video.moved.erase(found);
        SetEntry(page.title, HostEntry(page.title));
        for (std::uint32_t index = 0; index < page.extraCount; ++index) SetEntry(page.extra[index], HostEntry(page.extra[index]));
        if (page.own) {
            VirtualProtect(reinterpret_cast<void*>(source), kTitlePageBytes, page.protection, &previous);
            video.heldOwnBytes -= kTitlePageBytes;
        } else {
            video.setup.guard(source, 0, nullptr, 0);
        }
        for (std::uint32_t index = 0; index < page.viewCount; ++index) {
            const auto view = video.guardedViews.find(page.views[index]);
            if (view != video.guardedViews.end() && view->second == source) video.guardedViews.erase(view);
        }
        block.free.push_back(page.page);
        video.heldBytes -= kTitlePageBytes;
        ++video.report.returnedPages[static_cast<std::size_t>(reason)];
    }
    video.report.copyNanoseconds += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
}

void TitleAddressSpace::ReturnPages(const std::vector<std::uint64_t>& listed, PageReturn reason) {
    std::vector<std::uint64_t> sources;
    for (const std::uint64_t source : listed) {
        const auto found = video_->moved.find(source);
        if (found != video_->moved.end() && found->second.readOnly) {
            DropReadPage(source, reason, 0, 0);
        } else {
            sources.push_back(source);
        }
    }
    if (sources.empty()) return;
    const TimedScope timed(TimedPart::AddressIdleWait);
    StageOfThisThread(kWaitsForQueueLock);
    std::lock_guard<std::mutex> queue(device_.QueueLock());
    StageOfThisThread(kWaitsForGpu);
    const auto begin = std::chrono::steady_clock::now();
    const bool idle = WaitForQueue(kReturnWaitNanoseconds);
    if (!idle) ++video_->report.unsyncedReturns;
    video_->report.waitNanoseconds += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
    ReturnPagesHeld(sources, reason, idle);
    StageOfThisThread(kHoldsLock);
    ++video_->report.returnEvents[static_cast<std::size_t>(reason)];
}

void TitleAddressSpace::ReturnForUse(std::uint64_t source, PageReturn reason, std::uint64_t address, std::uint64_t code) {
    VideoPages& video = *video_;
    const auto found = video.moved.find(source);
    if (found == video.moved.end()) return;
    if (found->second.readOnly) {
        DropReadPage(source, reason, address, code);
        return;
    }
    const std::uint64_t group = found->second.group;
    VideoPageEvent event;
    event.reason = reason;
    event.address = address;
    event.code = code;
    if (reason == PageReturn::UsedByHandle) event.command = t_namingCommand;
    std::vector<std::uint64_t> sources;
    const auto live = video.byId.find(group);
    if (live != video.byId.end()) {
        event.buffer = live->second->first;
        event.bufferBytes = live->second->second.bytes;
    }
    if (video.setup.wholeBuffer && live != video.byId.end()) {
        live->second->second.home = true;
        for (const auto& [other, page] : video.moved) {
            if (page.group == group) sources.push_back(other);
        }
    } else {
        sources.push_back(source);
        video.pinned.insert(source);
    }
    event.pages = sources.size();
    ReturnPages(sources, reason);
    AddEvent(event);
}

void TitleAddressSpace::PinSource(std::uint64_t source, PageReturn reason, std::uint64_t address) {
    const std::uint64_t page = source & ~(kTitlePageBytes - 1);
    video_->pinned.insert(page);
    if (!video_->moved.empty()) ReturnForUse(page, reason, address, 0);
}

void TitleAddressSpace::PinRange(std::map<std::uint64_t, Range>::iterator range, std::uint64_t address, std::uint64_t bytes) {
    if (range->second.imported.memory != VK_NULL_HANDLE) {
        PinOwn(address, bytes, PageReturn::UsedByHandle, address);
        return;
    }
    if (range->second.named) return;
    range->second.named = true;
    const std::uint64_t end = range->first + range->second.bytes;
    for (auto host = hostPages_.lower_bound(range->first & ~(kTitlePageBytes - 1)); host != hostPages_.end() && host->first < end; ++host) {
        PinSource(host->second, PageReturn::UsedByHandle, address);
    }
}

void TitleAddressSpace::PinSpan(std::uint64_t start, std::uint64_t bytes) {
    const std::uint64_t end = start + bytes;
    for (auto it = FirstOverlap(start); it != ranges_.end() && it->first < end; ++it) {
        const std::uint64_t from = (std::max)(start, it->first);
        PinRange(it, from, (std::min)(end, it->first + it->second.bytes) - from);
    }
}

bool TitleAddressSpace::OwnPinned(std::uint64_t title) const {
    const auto& pins = video_->ownPinned;
    auto it = pins.upper_bound(title);
    if (it == pins.begin()) return false;
    --it;
    return it->second > title;
}

void TitleAddressSpace::CutOwnPins(std::uint64_t start, std::uint64_t end) {
    auto& pins = video_->ownPinned;
    auto it = pins.upper_bound(start);
    if (it != pins.begin()) --it;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> kept;
    while (it != pins.end() && it->first < end) {
        if (it->second <= start) {
            ++it;
            continue;
        }
        if (it->first < start) kept.emplace_back(it->first, start);
        if (it->second > end) kept.emplace_back(end, it->second);
        it = pins.erase(it);
    }
    for (const auto& [first, last] : kept) pins[first] = last;
}

void TitleAddressSpace::PinOwn(std::uint64_t start, std::uint64_t bytes, PageReturn reason, std::uint64_t address) {
    if (bytes == 0) return;
    VideoPages& video = *video_;
    std::uint64_t first = AlignDown(start, kTitlePageBytes);
    std::uint64_t last = AlignUp(start + bytes, kTitlePageBytes);
    auto it = video.ownPinned.upper_bound(first);
    if (it != video.ownPinned.begin()) {
        const auto previous = std::prev(it);
        if (previous->second >= last) return;
    }
    std::vector<std::uint64_t> held;
    for (auto moved = video.moved.lower_bound(first); moved != video.moved.end() && moved->first < last; ++moved) {
        if (moved->second.own) held.push_back(moved->first);
    }
    if (it != video.ownPinned.begin()) {
        const auto previous = std::prev(it);
        if (previous->second >= first) {
            first = previous->first;
            last = (std::max)(last, previous->second);
            it = video.ownPinned.erase(previous);
        }
    }
    while (it != video.ownPinned.end() && it->first <= last) {
        last = (std::max)(last, it->second);
        it = video.ownPinned.erase(it);
    }
    video.ownPinned[first] = last;
    for (const std::uint64_t source : held) ReturnForUse(source, reason, address, 0);
}

bool TitleAddressSpace::OwnRange(std::uint64_t title) const {
    auto it = ranges_.upper_bound(title);
    if (it == ranges_.begin()) return false;
    --it;
    const std::uint64_t within = title - it->first;
    return it->second.imported.memory != VK_NULL_HANDLE && it->second.buffer != VK_NULL_HANDLE && within < it->second.bytes && it->second.bytes - within >= kTitlePageBytes;
}

void TitleAddressSpace::ReturnOwnSpan(std::uint64_t start, std::uint64_t end, PageReturn reason) {
    VideoPages& video = *video_;
    std::vector<std::uint64_t> leaving;
    for (auto moved = video.moved.lower_bound(AlignDown(start, kTitlePageBytes)); moved != video.moved.end() && moved->first < end; ++moved) {
        if (moved->second.own) leaving.push_back(moved->first);
    }
    Reconsider(leaving);
    ReturnPages(leaving, reason);
}

void TitleAddressSpace::Reconsider(const std::vector<std::uint64_t>& sources) {
    VideoPages& video = *video_;
    for (const std::uint64_t source : sources) {
        const auto moved = video.moved.find(source);
        if (moved == video.moved.end()) continue;
        const auto live = video.byId.find(moved->second.group);
        if (live != video.byId.end()) live->second->second.complete = false;
    }
}

bool TitleAddressSpace::UseOwnRanges(const std::vector<Candidate>& candidates) {
    VideoPages& video = *video_;
    const DeviceFunctions& vk = device_.Vk();
    std::vector<std::pair<Range*, VkDeviceSize>> fresh;
    for (const Candidate& candidate : candidates) {
        if (!candidate.own) continue;
        auto it = ranges_.upper_bound(candidate.title);
        if (it == ranges_.begin()) continue;
        --it;
        const std::uint64_t within = candidate.title - it->first;
        if (it->second.used || it->second.imported.memory == VK_NULL_HANDLE || within >= it->second.bytes || it->second.bytes - within < kTitlePageBytes) continue;
        const bool listed = std::any_of(fresh.begin(), fresh.end(), [&](const auto& entry) { return entry.first == &it->second; });
        if (!listed) fresh.emplace_back(&it->second, within);
    }
    if (fresh.empty()) return true;
    if (!video.scratchTaken) {
        if (!TakeVideoPage(video.scratchBlock, video.scratchPage)) return false;
        video.scratchTaken = true;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vk.resetCommandPool(device_.Handle(), video.pool, 0) != VK_SUCCESS || vk.beginCommandBuffer(video.commands, &begin) != VK_SUCCESS) return false;
    for (const auto& [range, within] : fresh) {
        const VkBufferCopy region{within, std::uint64_t{video.scratchPage} * kTitlePageBytes, kTitlePageBytes};
        vk.cmdCopyBuffer(video.commands, range->buffer, video.blocks[video.scratchBlock].allocation.buffer, 1, &region);
    }
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (vk.endCommandBuffer(video.commands) != VK_SUCCESS || vk.createFence(device_.Handle(), &fenceInfo, nullptr, &fence) != VK_SUCCESS) return false;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &video.commands;
    if (vk.queueSubmit(device_.Queue(), 1, &submit, fence) != VK_SUCCESS) {
        vk.destroyFence(device_.Handle(), fence, nullptr);
        return false;
    }
    VkResult waited = VK_TIMEOUT;
    while (waited == VK_TIMEOUT) waited = vk.waitForFences(device_.Handle(), 1, &fence, VK_TRUE, kReturnWaitNanoseconds);
    vk.destroyFence(device_.Handle(), fence, nullptr);
    if (waited != VK_SUCCESS) return false;
    for (const auto& [range, within] : fresh) range->used = true;
    return true;
}

bool TitleAddressSpace::UseOwnRangesLater(const std::vector<std::uint64_t>& starts) {
    VideoPages& video = *video_;
    const DeviceFunctions& vk = device_.Vk();
    if (video.useFence != VK_NULL_HANDLE) {
        if (vk.getFenceStatus(device_.Handle(), video.useFence) != VK_SUCCESS) return true;
        vk.destroyFence(device_.Handle(), video.useFence, nullptr);
        video.useFence = VK_NULL_HANDLE;
        for (const auto& [start, memory] : video.usePending) {
            const auto range = ranges_.find(start);
            if (range != ranges_.end() && range->second.imported.memory == memory) range->second.used = true;
        }
        video.usePending.clear();
    }
    std::vector<std::map<std::uint64_t, Range>::iterator> fresh;
    for (const std::uint64_t start : starts) {
        const auto range = ranges_.find(start);
        if (range != ranges_.end() && !range->second.used && range->second.imported.memory != VK_NULL_HANDLE && range->second.buffer != VK_NULL_HANDLE) fresh.push_back(range);
    }
    if (fresh.empty()) return true;
    if (video.useCommands == VK_NULL_HANDLE) return false;
    if (!video.scratchTaken) {
        if (!TakeVideoPage(video.scratchBlock, video.scratchPage)) return false;
        video.scratchTaken = true;
    }
    std::unique_lock<std::mutex> queue(device_.QueueLock(), std::try_to_lock);
    if (!queue.owns_lock()) return false;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vk.resetCommandPool(device_.Handle(), video.usePool, 0) != VK_SUCCESS || vk.beginCommandBuffer(video.useCommands, &begin) != VK_SUCCESS) return false;
    for (const auto& range : fresh) {
        const VkBufferCopy region{range->second.bufferOffset, std::uint64_t{video.scratchPage} * kTitlePageBytes, (std::min)(kTitlePageBytes, range->second.bytes)};
        vk.cmdCopyBuffer(video.useCommands, range->second.buffer, video.blocks[video.scratchBlock].allocation.buffer, 1, &region);
    }
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (vk.endCommandBuffer(video.useCommands) != VK_SUCCESS || vk.createFence(device_.Handle(), &fenceInfo, nullptr, &fence) != VK_SUCCESS) return false;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &video.useCommands;
    if (vk.queueSubmit(device_.Queue(), 1, &submit, fence) != VK_SUCCESS) {
        vk.destroyFence(device_.Handle(), fence, nullptr);
        return false;
    }
    video.useFence = fence;
    for (const auto& range : fresh) video.usePending.emplace_back(range->first, range->second.imported.memory);
    return true;
}

bool TitleAddressSpace::GuardOwnPage(std::uint64_t title, void* copy, std::uint32_t& protection) {
    void* const page = reinterpret_cast<void*>(title);
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(page, &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT || region.Type != MEM_PRIVATE ||
        (region.Protect != PAGE_READWRITE && region.Protect != PAGE_READONLY) ||
        reinterpret_cast<std::uint64_t>(region.BaseAddress) + region.RegionSize < title + kTitlePageBytes) {
        return false;
    }
    video_->everGuarded.insert(title);
    DWORD previous = 0;
    if (region.Protect != PAGE_READONLY && !VirtualProtect(page, kTitlePageBytes, PAGE_READONLY, &previous)) return false;
    std::memcpy(copy, page, kTitlePageBytes);
    if (!VirtualProtect(page, kTitlePageBytes, PAGE_NOACCESS, &previous)) {
        VirtualProtect(page, kTitlePageBytes, region.Protect, &previous);
        return false;
    }
    protection = region.Protect;
    return true;
}

void TitleAddressSpace::NoteAlias(std::uint64_t start, std::uint64_t bytes, std::uint64_t source, bool gpuMapped) {
    VideoPages& video = *video_;
    std::vector<std::uint64_t> leaving;
    const auto due = std::chrono::steady_clock::now() + std::chrono::seconds(video.setup.readSettleSeconds);
    for (std::uint64_t page = 0; page < bytes; page += kTitlePageBytes) {
        const std::uint64_t title = start + page;
        const std::uint64_t from = source + page;
        const auto old = hostPages_.find(title);
        if (old != hostPages_.end() && old->second == from) {
            if (gpuMapped && video.gpuMapped.insert(title).second && video.setup.readCapBytes != 0) DeferRead(from, due);
            continue;
        }
        if (old != hostPages_.end()) {
            if (video.moved.count(old->second) != 0) leaving.push_back(old->second);
            ForgetTitle(old->second, title);
        }
        if (video.moved.count(from) != 0) leaving.push_back(from);
        if (video.moved.count(title) != 0) leaving.push_back(title);
        video.titlesOf.emplace(from, title);
        if (gpuMapped) {
            video.gpuMapped.insert(title);
            if (video.setup.readCapBytes != 0) DeferRead(from, due);
        } else {
            video.gpuMapped.erase(title);
        }
    }
    std::sort(leaving.begin(), leaving.end());
    leaving.erase(std::unique(leaving.begin(), leaving.end()), leaving.end());
    Reconsider(leaving);
    ReturnPages(leaving, PageReturn::Remapped);
}

void TitleAddressSpace::NoteRelease(std::uint64_t start, std::uint64_t end) {
    VideoPages& video = *video_;
    std::vector<std::uint64_t> leaving;
    for (auto it = hostPages_.lower_bound(start & ~(kTitlePageBytes - 1)); it != hostPages_.end() && it->first < end; ++it) {
        if (video.moved.count(it->second) != 0) leaving.push_back(it->second);
        ForgetTitle(it->second, it->first);
        video.gpuMapped.erase(it->first);
        if (video.titlesOf.count(it->second) == 0) {
            video.pinned.erase(it->second);
            video.readStates.erase(it->second);
        }
    }
    for (auto it = FirstOverlap(start); it != ranges_.end() && it->first < end; ++it) {
        if (it->second.imported.memory == VK_NULL_HANDLE) continue;
        const std::uint64_t rangeEnd = it->first + it->second.bytes;
        for (auto moved = video.moved.lower_bound(AlignDown(it->first, kTitlePageBytes)); moved != video.moved.end() && moved->first < rangeEnd; ++moved) {
            if (moved->second.own) leaving.push_back(moved->first);
        }
    }
    std::sort(leaving.begin(), leaving.end());
    leaving.erase(std::unique(leaving.begin(), leaving.end()), leaving.end());
    Reconsider(leaving);
    ReturnPages(leaving, PageReturn::Unmapped);
    CutOwnPins(AlignDown(start, kTitlePageBytes), AlignUp(end, kTitlePageBytes));
    video.pinned.erase(video.pinned.lower_bound(AlignDown(start, kTitlePageBytes)), video.pinned.lower_bound(AlignUp(end, kTitlePageBytes)));
    video.ownStates.erase(video.ownStates.lower_bound(AlignDown(start, kTitlePageBytes)), video.ownStates.lower_bound(AlignUp(end, kTitlePageBytes)));
}

void TitleAddressSpace::NoteBackingRemoved(std::uint64_t view, std::uint64_t bytes) {
    VideoPages& video = *video_;
    std::vector<std::uint64_t> leaving;
    for (auto it = video.moved.lower_bound(view); it != video.moved.end() && it->first < view + bytes; ++it) leaving.push_back(it->first);
    ReturnPages(leaving, PageReturn::Unmapped);
    video.pinned.erase(video.pinned.lower_bound(view), video.pinned.lower_bound(view + bytes));
}

void TitleAddressSpace::WatchBuffer(std::uint64_t address, std::uint64_t bytes, bool added) {
    Doing doing("a buffer the GPU may write is created or released", address);
    std::lock_guard<std::mutex> guard(lock_);
    doing.Stage(kHoldsLock);
    if (!video_) return;
    VideoPages& video = *video_;
    video.watchedSpansStale = true;
    if (added) {
        DropReadSpan(address, address + bytes);
        WatchedBuffer buffer;
        buffer.bytes = bytes;
        buffer.id = video.nextId++;
        buffer.added = std::chrono::steady_clock::now();
        video.byId[buffer.id] = video.buffers.emplace(address, buffer);
        return;
    }
    const auto [first, last] = video.buffers.equal_range(address);
    for (auto it = first; it != last; ++it) {
        if (it->second.bytes != bytes) continue;
        video.byId.erase(it->second.id);
        video.buffers.erase(it);
        return;
    }
}

bool TitleAddressSpace::Movable(const Candidate& candidate) const {
    const VideoPages& video = *video_;
    const auto live = video.byId.find(candidate.group);
    if (live == video.byId.end() || live->second->second.home) return false;
    if (video.pinned.count(candidate.source) != 0 || video.moved.count(candidate.source) != 0 || video.movedTitles.count(candidate.title) != 0) return false;
    const auto host = hostPages_.find(candidate.title);
    if (candidate.own) return host == hostPages_.end() && candidate.source == candidate.title && OwnRange(candidate.title) && !OwnPinned(candidate.title);
    if (host == hostPages_.end() || host->second != candidate.source) return false;
    const auto [first, last] = video.titlesOf.equal_range(candidate.source);
    std::uint32_t titles = 0;
    for (auto it = first; it != last; ++it, ++titles) {
        const auto other = hostPages_.find(it->second);
        if (other == hostPages_.end() || other->second != candidate.source || video.movedTitles.count(it->second) != 0) return false;
    }
    return titles >= 1 && titles <= kVideoPageTitles;
}

void TitleAddressSpace::CollectVideoWork(std::vector<Candidate>& candidates, std::vector<std::uint64_t>& leaving, std::vector<SettledBuffer>& settled) {
    VideoPages& video = *video_;
    const auto now = std::chrono::steady_clock::now();
    for (auto& [source, page] : video.moved) {
        if (page.readOnly || video.byId.count(page.group) != 0) continue;
        bool adopted = false;
        for (std::uint32_t index = 0; index <= page.extraCount && !adopted; ++index) {
            const std::uint64_t title = index == 0 ? page.title : page.extra[index - 1];
            auto it = video.buffers.upper_bound(title);
            for (int looked = 0; it != video.buffers.begin() && looked < 8 && !adopted; ++looked) {
                --it;
                if (!it->second.home && it->first <= title && title + kTitlePageBytes <= it->first + it->second.bytes) {
                    page.group = it->second.id;
                    adopted = true;
                }
            }
        }
        if (!adopted) leaving.push_back(source);
    }
    const std::uint64_t room = (std::min)(VideoRoom() + leaving.size() * kTitlePageBytes, kMoveRoundBytes);
    std::uint64_t waiting = 0;
    std::uint64_t other = 0;
    std::uint64_t overCap = 0;
    std::unordered_set<std::uint64_t> taken;
    for (auto& [address, buffer] : video.buffers) {
        if (buffer.home || buffer.complete || now - buffer.added < std::chrono::seconds(video.setup.settleSeconds)) continue;
        const std::uint64_t whole = AlignUp(address, kTitlePageBytes);
        const std::uint64_t wholeEnd = AlignDown(address + buffer.bytes, kTitlePageBytes);
        const std::uint64_t first = video.setup.partialPages ? AlignDown(address, kTitlePageBytes) : whole;
        const std::uint64_t last = video.setup.partialPages ? AlignUp(address + buffer.bytes, kTitlePageBytes) : wholeEnd;
        bool decided = true;
        std::uint64_t shared = 0;
        std::uint64_t pinned = 0;
        for (std::uint64_t title = first; title < last; title += kTitlePageBytes) {
            if (video.movedTitles.count(title) != 0) continue;
            const bool partial = title < whole || title >= wholeEnd;
            const auto host = hostPages_.find(title);
            if (host == hostPages_.end()) {
                if (!OwnRange(title)) {
                    auto range = ranges_.upper_bound(title);
                    const bool mapped = range != ranges_.begin() && title - std::prev(range)->first < std::prev(range)->second.bytes;
                    ++(mapped ? other : waiting);
                    decided = false;
                    continue;
                }
                if (OwnPinned(title) || video.pinned.count(title) != 0) {
                    ++pinned;
                    continue;
                }
                DropReadPage(title, PageReturn::GpuMayWrite, title, 0);
                if (video.moved.count(title) != 0) continue;
                if (candidates.size() * kTitlePageBytes >= room) {
                    ++overCap;
                    decided = false;
                    continue;
                }
                candidates.push_back(Candidate{title, title, buffer.id, true, partial});
                continue;
            }
            const std::uint64_t source = host->second;
            if (video.pinned.count(source) != 0) {
                ++pinned;
                continue;
            }
            DropReadPage(source, PageReturn::GpuMayWrite, title, 0);
            if (video.moved.count(source) != 0 || taken.count(source) != 0) continue;
            const std::size_t titles = video.titlesOf.count(source);
            if ((source & (kTitlePageBytes - 1)) != 0 || titles == 0 || titles > kVideoPageTitles) {
                ++shared;
                continue;
            }
            if (candidates.size() * kTitlePageBytes >= room) {
                ++overCap;
                decided = false;
                continue;
            }
            taken.insert(source);
            candidates.push_back(Candidate{title, source, buffer.id, false, partial});
        }
        if (decided) settled.push_back(SettledBuffer{buffer.id, shared, pinned});
    }
    video.report.waitingPages = waiting;
    video.report.otherPages = other;
    video.report.overCapPages = overCap;
}

std::uint64_t TitleAddressSpace::ArenaRoom() const {
    std::uint64_t room = video_->arenaCap - (std::min)(video_->arenaCap, video_->arenaBytes);
    for (const VideoBlock& block : video_->blocks) room += block.free.size() * kTitlePageBytes;
    return room;
}

std::uint64_t TitleAddressSpace::VideoRoom() const {
    const std::uint64_t held = video_->heldBytes - video_->heldReadBytes;
    return (std::min)(ArenaRoom(), video_->setup.capBytes - (std::min)(video_->setup.capBytes, held));
}

std::uint64_t TitleAddressSpace::ReadRoom() const {
    if (video_->readStopped) return 0;
    const std::uint64_t held = video_->heldReadBytes - video_->heldOwnReadBytes;
    return (std::min)(ArenaRoom(), video_->setup.readCapBytes - (std::min)(video_->setup.readCapBytes, held));
}

std::uint64_t TitleAddressSpace::ReadOwnRoom() const {
    if (video_->readStopped) return 0;
    return (std::min)(ArenaRoom(), video_->setup.readOwnCapBytes - (std::min)(video_->setup.readOwnCapBytes, video_->heldOwnReadBytes));
}

void TitleAddressSpace::DeferRead(std::uint64_t source, std::chrono::steady_clock::time_point due) {
    video_->readStates[source].due = due;
    video_->readDue.emplace(due, source);
}

bool TitleAddressSpace::GpuMayWrite(std::uint64_t title) {
    VideoPages& video = *video_;
    const std::uint64_t end = title + kTitlePageBytes;
    const auto written = video.gpuWritten.lower_bound(end);
    if (written != video.gpuWritten.begin() && std::prev(written)->second > title) return true;
    if (video.watchedSpansStale) {
        video.watchedSpans.clear();
        for (const auto& [address, buffer] : video.buffers) {
            const std::uint64_t last = address + buffer.bytes;
            if (!video.watchedSpans.empty() && address <= video.watchedSpans.back().second) {
                video.watchedSpans.back().second = (std::max)(video.watchedSpans.back().second, last);
            } else {
                video.watchedSpans.emplace_back(address, last);
            }
        }
        video.watchedSpansStale = false;
    }
    const auto span = std::lower_bound(video.watchedSpans.begin(), video.watchedSpans.end(), std::make_pair(end, std::uint64_t{0}));
    return span != video.watchedSpans.begin() && std::prev(span)->second > title;
}

int TitleAddressSpace::ReadCandidate(std::uint64_t source) {
    VideoPages& video = *video_;
    if ((source & (kTitlePageBytes - 1)) != 0 || video.moved.count(source) != 0) return kReadNever;
    if (video.pinned.count(source) != 0) {
        ++video.report.readSkippedPinned;
        return kReadNever;
    }
    const auto [first, last] = video.titlesOf.equal_range(source);
    std::uint32_t titles = 0;
    bool mapped = false;
    for (auto it = first; it != last; ++it, ++titles) {
        const auto host = hostPages_.find(it->second);
        if (host == hostPages_.end() || host->second != source || video.movedTitles.count(it->second) != 0) return kReadNever;
        mapped = mapped || video.gpuMapped.count(it->second) != 0;
        if (GpuMayWrite(it->second)) {
            ++video.report.readSkippedWritable;
            return kReadNever;
        }
    }
    if (titles == 0 || !mapped) return kReadNever;
    if (titles > kVideoPageTitles) {
        ++video.report.readSkippedShared;
        return kReadNever;
    }
    return kReadNow;
}

bool TitleAddressSpace::MoveReadPage(std::uint64_t source) {
    VideoPages& video = *video_;
    std::uint32_t block = 0;
    std::uint32_t slot = 0;
    if (!TakeVideoPage(block, slot)) {
        DeferRead(source, std::chrono::steady_clock::now() + kReadRoomPause);
        return false;
    }
    MovedPage moved;
    moved.block = block;
    moved.page = slot;
    moved.readOnly = true;
    const std::uint32_t views = video.setup.guard(source, kVideoPageGuardWrites, moved.views, kVideoPageViews);
    if (views == kVideoPageGuardRefused || views > kVideoPageViews) {
        video.blocks[block].free.push_back(slot);
        ++video.report.readRefusedPages;
        return false;
    }
    moved.viewCount = views;
    void* const held = static_cast<std::uint8_t*>(video.blocks[block].allocation.mapped) + std::uint64_t{slot} * kTitlePageBytes;
    std::memcpy(held, reinterpret_cast<const void*>(source), kTitlePageBytes);
    const VkDeviceAddress device = video.blocks[block].allocation.address + std::uint64_t{slot} * kTitlePageBytes;
    const auto [first, last] = video.titlesOf.equal_range(source);
    bool named = false;
    for (auto it = first; it != last; ++it) {
        if (!named) {
            moved.title = it->second;
            named = true;
        } else if (moved.extraCount < kVideoPageTitles - 1) {
            moved.extra[moved.extraCount++] = it->second;
        }
    }
    video.moved[source] = moved;
    video.movedTitles[moved.title] = device;
    for (std::uint32_t index = 0; index < moved.extraCount; ++index) video.movedTitles[moved.extra[index]] = device;
    for (std::uint32_t index = 0; index < views; ++index) {
        video.guardedViews[moved.views[index]] = source;
        video.everGuarded.insert(moved.views[index]);
    }
    SetEntry(moved.title, device);
    for (std::uint32_t index = 0; index < moved.extraCount; ++index) SetEntry(moved.extra[index], device);
    video.heldBytes += kTitlePageBytes;
    video.heldReadBytes += kTitlePageBytes;
    ++video.report.readMovedPages;
    return true;
}

void TitleAddressSpace::DropReadPage(std::uint64_t source, PageReturn reason, std::uint64_t address, std::uint64_t code) {
    VideoPages& video = *video_;
    const auto found = video.moved.find(source);
    if (found == video.moved.end() || !found->second.readOnly) return;
    const MovedPage page = found->second;
    VideoBlock& block = video.blocks[page.block];
    const std::uint8_t* const held = static_cast<const std::uint8_t*>(block.allocation.mapped) + std::uint64_t{page.page} * kTitlePageBytes;
    const bool written = reason == PageReturn::CpuAccess || reason == PageReturn::GivenOut || reason == PageReturn::SystemCall;
    const bool gone = reason == PageReturn::Unmapped || reason == PageReturn::Remapped;
    bool open = true;
    if (page.own) {
        MEMORY_BASIC_INFORMATION region{};
        open = VirtualQuery(reinterpret_cast<const void*>(source), &region, sizeof(region)) == sizeof(region) && region.State == MEM_COMMIT && region.Protect == PAGE_READONLY &&
               reinterpret_cast<std::uint64_t>(region.BaseAddress) + region.RegionSize >= source + kTitlePageBytes;
    }
    bool differed = false;
    if (!gone && open) {
        ++video.readDrops;
        if (video.readDiffered != 0 || video.readDrops <= kReadChecksAlways || video.readDrops % (written ? kReadCheckEvery : kReadCheckEveryRead) == 0) {
            ++(page.own ? video.report.readOwnCheckedPages : video.report.readCheckedPages);
            video.staging.resize(kTitlePageBytes);
            std::memcpy(video.staging.data(), held, kTitlePageBytes);
            differed = std::memcmp(video.staging.data(), reinterpret_cast<const void*>(source), kTitlePageBytes) != 0;
            if (differed) {
                ++(page.own ? video.report.readOwnDifferedPages : video.report.readDifferedPages);
                ++video.readDiffered;
                video.pinned.insert(source);
                if (video.readDiffered >= kReadDifferencesAllowed) video.readStopped = true;
            }
        }
    }
    DWORD previous = 0;
    DWORD current = PAGE_READONLY;
    if (page.own && open && differed) {
        if (VirtualProtect(reinterpret_cast<void*>(source), kTitlePageBytes, PAGE_READWRITE, &previous)) {
            current = PAGE_READWRITE;
            std::memcpy(reinterpret_cast<void*>(source), video.staging.data(), kTitlePageBytes);
        } else {
            ++video.report.lostPages;
        }
    } else if (differed) {
        std::memcpy(reinterpret_cast<void*>(source), video.staging.data(), kTitlePageBytes);
    }
    video.movedTitles.erase(page.title);
    for (std::uint32_t index = 0; index < page.extraCount; ++index) video.movedTitles.erase(page.extra[index]);
    video.moved.erase(found);
    SetEntry(page.title, HostEntry(page.title));
    for (std::uint32_t index = 0; index < page.extraCount; ++index) SetEntry(page.extra[index], HostEntry(page.extra[index]));
    if (page.own) {
        if (open && page.protection != current) VirtualProtect(reinterpret_cast<void*>(source), kTitlePageBytes, page.protection, &previous);
        SetHeldMark(source, false);
        video.heldOwnReadBytes -= kTitlePageBytes;
    } else {
        video.setup.guard(source, 0, nullptr, 0);
    }
    for (std::uint32_t index = 0; index < page.viewCount; ++index) {
        const auto view = video.guardedViews.find(page.views[index]);
        if (view != video.guardedViews.end() && view->second == source) video.guardedViews.erase(view);
    }
    video.freed.push_back(FreedPage{page.block, page.page});
    video.heldBytes -= kTitlePageBytes;
    video.heldReadBytes -= kTitlePageBytes;
    ++(page.own ? video.report.readOwnReturnedPages : video.report.readReturnedPages)[static_cast<std::size_t>(reason)];
    if (written && !differed) {
        ReadState& state = page.own ? video.ownStates[source] : video.readStates[source];
        ++state.writes;
        if (state.writes == 2) ++(page.own ? video.report.readOwnWrittenTwice : video.report.readWrittenTwice);
        const std::uint32_t steps = (std::min)(state.writes, kReadPageBackoffSteps);
        const auto due = std::chrono::steady_clock::now() + std::chrono::seconds(std::uint64_t{video.setup.readSettleSeconds} << steps);
        if (page.own) {
            state.due = due;
        } else {
            DeferRead(source, due);
        }
    } else if (!page.own && gone && video.titlesOf.count(source) != 0) {
        DeferRead(source, std::chrono::steady_clock::now() + std::chrono::seconds(video.setup.readSettleSeconds));
    } else if (page.own && gone) {
        video.ownStates.erase(source);
    }
    const bool cpu = reason == PageReturn::CpuAccess || reason == PageReturn::SystemCall;
    std::uint64_t& events = page.own ? video.ownWriteEvents : video.readWriteEvents;
    if (cpu) ++events;
    if (differed || (cpu && (events <= kReadWriteEventsAlways || (events & (events - 1)) == 0))) {
        VideoPageEvent event;
        event.reason = reason;
        event.buffer = page.title;
        event.bufferBytes = kTitlePageBytes;
        event.pages = differed ? 0 : 1;
        event.address = address;
        event.code = code;
        event.readOnly = true;
        event.own = page.own;
        event.count = differed ? video.readDiffered : events;
        AddEvent(event);
    }
}

void TitleAddressSpace::DropReadSpan(std::uint64_t start, std::uint64_t end) {
    VideoPages& video = *video_;
    if (video.heldReadBytes == 0) return;
    for (std::uint64_t title = AlignDown(start, kTitlePageBytes); title < end; title += kTitlePageBytes) {
        if (video.movedTitles.count(title) == 0) continue;
        const auto host = hostPages_.find(title);
        DropReadPage(host != hostPages_.end() ? host->second : title, PageReturn::GpuMayWrite, title, 0);
    }
}

TitleAddressSpace::Naming::Naming(const char* command) : previous_(t_namingCommand) {
    t_namingCommand = command;
}

TitleAddressSpace::Naming::~Naming() {
    t_namingCommand = previous_;
}

bool TitleAddressSpace::Covered(std::uint64_t address, std::uint64_t bytes) const {
    const std::uint64_t end = address + bytes;
    auto it = ranges_.upper_bound(address);
    if (it == ranges_.begin()) return false;
    --it;
    std::uint64_t at = address;
    while (at < end) {
        if (it == ranges_.end() || it->first > at || at - it->first >= it->second.bytes) return false;
        at = it->first + it->second.bytes;
        ++it;
    }
    return true;
}

bool TitleAddressSpace::TableCommand(const TitleAccess* accesses, std::size_t count) {
    constexpr std::uint64_t kMostBytes = 256ull << 20;
    if (!tableCommands_.load(std::memory_order_relaxed) || count == 0) return false;
    Doing doing("a command on title memory is checked for running through the page table", accesses[0].address);
    std::lock_guard<std::mutex> guard(lock_);
    doing.Stage(kHoldsLock);
    if (!video_) return false;
    VideoPages& video = *video_;
    bool wanted = false;
    bool fits = true;
    std::uint64_t total = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const TitleAccess& access = accesses[index];
        if (access.bytes == 0 || access.address + access.bytes < access.address) return false;
        const std::uint64_t end = access.address + (std::min)(access.bytes, kMostBytes);
        for (std::uint64_t title = AlignDown(access.address, kTitlePageBytes); title < end && !wanted; title += kTitlePageBytes) {
            wanted = GpuMayWrite(title);
        }
        fits = fits && access.address % 4 == 0 && access.bytes % 4 == 0 && access.bytes <= kMostBytes && Covered(access.address, access.bytes);
        for (std::size_t other = 0; other < index; ++other) {
            const bool apart = accesses[other].address + accesses[other].bytes <= access.address || access.address + access.bytes <= accesses[other].address;
            fits = fits && (apart || (!access.write && !accesses[other].write));
        }
        total += access.bytes;
    }
    if (!wanted) return false;
    if (!fits) {
        ++video.report.tableRefused;
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (accesses[index].write) MarkWritten(accesses[index].address, accesses[index].bytes);
    }
    ++video.report.tableCommands;
    video.report.tableBytes += total;
    return true;
}

void TitleAddressSpace::MarkGpuWritten(std::uint64_t address, std::uint64_t bytes) {
    if (bytes == 0) return;
    Doing doing("a view the GPU may write is created", address);
    std::lock_guard<std::mutex> guard(lock_);
    doing.Stage(kHoldsLock);
    if (video_) MarkWritten(address, bytes);
}

void TitleAddressSpace::MarkWritten(std::uint64_t address, std::uint64_t bytes) {
    if (video_->setup.readCapBytes == 0 && video_->setup.readOwnCapBytes == 0) return;
    VideoPages& video = *video_;
    std::uint64_t first = address;
    std::uint64_t last = address + bytes;
    auto it = video.gpuWritten.upper_bound(first);
    if (it != video.gpuWritten.begin()) {
        const auto previous = std::prev(it);
        if (previous->second >= last) return;
        if (previous->second >= first) {
            first = previous->first;
            it = video.gpuWritten.erase(previous);
        }
    }
    while (it != video.gpuWritten.end() && it->first <= last) {
        last = (std::max)(last, it->second);
        it = video.gpuWritten.erase(it);
    }
    video.gpuWritten[first] = last;
    DropReadSpan(address, address + bytes);
}

void TitleAddressSpace::CollectFreedPages() {
    VideoPages& video = *video_;
    const DeviceFunctions& vk = device_.Vk();
    if (video.freedFence != VK_NULL_HANDLE) {
        if (vk.getFenceStatus(device_.Handle(), video.freedFence) != VK_SUCCESS) return;
        vk.destroyFence(device_.Handle(), video.freedFence, nullptr);
        video.freedFence = VK_NULL_HANDLE;
        for (const FreedPage& page : video.freedBehindFence) video.blocks[page.block].free.push_back(page.page);
        video.freedBehindFence.clear();
    }
    if (video.freed.empty()) return;
    std::unique_lock<std::mutex> queue(device_.QueueLock(), std::try_to_lock);
    if (!queue.owns_lock()) return;
    VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (vk.createFence(device_.Handle(), &info, nullptr, &fence) != VK_SUCCESS) return;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (vk.queueSubmit(device_.Queue(), 1, &submit, fence) != VK_SUCCESS) {
        vk.destroyFence(device_.Handle(), fence, nullptr);
        return;
    }
    video.freedFence = fence;
    video.freedBehindFence.swap(video.freed);
}

bool TitleAddressSpace::PumpReadPages() {
    Doing doing("the copier's round for read-only pages", 0);
    std::lock_guard<std::mutex> guard(lock_);
    doing.Stage(kHoldsLock);
    if (!video_ || video_->setup.readCapBytes == 0) return false;
    VideoPages& video = *video_;
    CollectFreedPages();
    const auto now = std::chrono::steady_clock::now();
    if (video.readDue.empty() || video.readDue.begin()->first > now) {
        video.readWaiting = 0;
        return false;
    }
    if (ReadRoom() < kTitlePageBytes) {
        if (now - video.readWaitingAt >= kReadWaitingCount) {
            video.readWaitingAt = now;
            video.readWaiting = static_cast<std::uint64_t>(std::distance(video.readDue.begin(), video.readDue.upper_bound(now)));
        }
        return false;
    }
    video.readWaiting = 0;
    everMoved_.store(true, std::memory_order_release);
    std::uint64_t movedBytes = 0;
    std::size_t looked = 0;
    bool more = false;
    std::vector<std::uint64_t> later;
    while (!video.readDue.empty() && video.readDue.begin()->first <= now) {
        if (looked == kReadRoundLooks || movedBytes >= kReadRoundBytes) {
            more = true;
            break;
        }
        const auto due = video.readDue.begin()->first;
        const std::uint64_t source = video.readDue.begin()->second;
        video.readDue.erase(video.readDue.begin());
        ++looked;
        const auto state = video.readStates.find(source);
        if (state == video.readStates.end() || state->second.due != due) continue;
        if (ReadCandidate(source) != kReadNow) continue;
        if (ReadRoom() < kTitlePageBytes) {
            later.push_back(source);
            continue;
        }
        if (MoveReadPage(source)) movedBytes += kTitlePageBytes;
    }
    for (const std::uint64_t source : later) DeferRead(source, now + kReadRoomPause);
    video.report.readCopyNanoseconds += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - now).count());
    return more;
}

int TitleAddressSpace::OwnReadCandidate(std::uint64_t title, std::chrono::steady_clock::time_point now) {
    VideoPages& video = *video_;
    if (video.moved.count(title) != 0 || video.movedTitles.count(title) != 0 || hostPages_.count(title) != 0 || !OwnRange(title)) return kReadNever;
    if (OwnPinned(title) || video.pinned.count(title) != 0) return kReadPinned;
    if (GpuMayWrite(title)) return kReadWritable;
    const auto [state, fresh] = video.ownStates.try_emplace(title);
    if (fresh) state->second.due = now + std::chrono::seconds(video.setup.readSettleSeconds);
    return state->second.due <= now ? kReadNow : kReadLater;
}

bool TitleAddressSpace::MoveOwnReadPage(std::uint64_t title) {
    VideoPages& video = *video_;
    void* const page = reinterpret_cast<void*>(title);
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(page, &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT || region.Type != MEM_PRIVATE ||
        (region.Protect != PAGE_READWRITE && region.Protect != PAGE_READONLY) || reinterpret_cast<std::uint64_t>(region.BaseAddress) + region.RegionSize < title + kTitlePageBytes) {
        video.pinned.insert(title);
        ++video.report.readOwnRefusedPages;
        return false;
    }
    std::uint32_t block = 0;
    std::uint32_t slot = 0;
    if (!TakeVideoPage(block, slot)) return false;
    DWORD previous = 0;
    video.everGuarded.insert(title);
    if (region.Protect != PAGE_READONLY && !VirtualProtect(page, kTitlePageBytes, PAGE_READONLY, &previous)) {
        video.blocks[block].free.push_back(slot);
        video.pinned.insert(title);
        ++video.report.readOwnRefusedPages;
        return false;
    }
    void* const held = static_cast<std::uint8_t*>(video.blocks[block].allocation.mapped) + std::uint64_t{slot} * kTitlePageBytes;
    std::memcpy(held, page, kTitlePageBytes);
    const VkDeviceAddress device = video.blocks[block].allocation.address + std::uint64_t{slot} * kTitlePageBytes;
    MovedPage moved;
    moved.title = title;
    moved.block = block;
    moved.page = slot;
    moved.own = true;
    moved.readOnly = true;
    moved.protection = region.Protect;
    moved.viewCount = 1;
    moved.views[0] = title;
    video.moved[title] = moved;
    video.movedTitles[title] = device;
    video.guardedViews[title] = title;
    SetEntry(title, device);
    SetHeldMark(title, true);
    video.heldBytes += kTitlePageBytes;
    video.heldReadBytes += kTitlePageBytes;
    video.heldOwnReadBytes += kTitlePageBytes;
    ++video.report.readOwnMovedPages;
    return true;
}

bool TitleAddressSpace::PumpOwnReadPages() {
    Doing doing("the copier's round for read-only copies of memory committed on its own", 0);
    std::lock_guard<std::mutex> guard(lock_);
    doing.Stage(kHoldsLock);
    if (!video_ || video_->setup.readOwnCapBytes == 0 || !useMarks_) return false;
    VideoPages& video = *video_;
    CollectFreedPages();
    const auto now = std::chrono::steady_clock::now();
    const std::uint8_t epoch = UseEpoch();
    const std::uint64_t total = video.markRegions.size() * kTitlePagesPerRegion;
    if (total == 0) return false;
    OwnSnapshot snapshot;
    std::vector<std::uint64_t> wanted[kUsedEpochs];
    std::vector<std::uint64_t> unused;
    std::size_t looked = 0;
    std::uint64_t resume = total;
    const std::uint64_t start = video.ownCursor % total;
    for (std::uint64_t step = 0; step < total; ++step) {
        const std::uint64_t at = (start + step) % total;
        const std::uint64_t region = video.markRegions[at / kTitlePagesPerRegion];
        UseMarks& mark = useMarks_[region].load(std::memory_order_relaxed)[at % kTitlePagesPerRegion];
        std::uint8_t value = mark.load(std::memory_order_relaxed);
        if (value == 0) continue;
        const std::uint64_t title = region * kTitleRegionBytes + (at % kTitlePagesPerRegion) * kTitlePageBytes;
        std::uint32_t age = MarkAge(value, epoch);
        while (age >= kStaleEpochs && age != kMarkNever) {
            if (mark.compare_exchange_weak(value, static_cast<std::uint8_t>(value & ~kMarkEpochs), std::memory_order_relaxed)) {
                value = static_cast<std::uint8_t>(value & ~kMarkEpochs);
                age = kMarkNever;
                break;
            }
            age = MarkAge(value, epoch);
        }
        if ((value & kMarkRootWrites) != 0) ++snapshot.rootWritten;
        const bool used = age < kUsedEpochs;
        if ((value & kMarkHeld) != 0) {
            if (used) {
                ++snapshot.used;
                ++snapshot.usedHeld;
            } else {
                unused.push_back(title);
            }
            continue;
        }
        if (!used) continue;
        ++snapshot.used;
        if ((value & kMarkRootWrites) != 0) {
            ++snapshot.writable;
            continue;
        }
        if (looked == kOwnRoundLooks) {
            if (resume == total) resume = at;
            continue;
        }
        ++looked;
        switch (OwnReadCandidate(title, now)) {
        case kReadNow: wanted[age].push_back(title); break;
        case kReadLater: ++snapshot.settling; break;
        case kReadPinned: ++snapshot.pinned; break;
        case kReadWritable: ++snapshot.writable; break;
        default: break;
        }
    }
    video.ownCursor = resume == total ? 0 : resume;
    snapshot.lookedAt = looked;
    std::vector<std::uint64_t> candidates;
    for (const std::vector<std::uint64_t>& bucket : wanted) candidates.insert(candidates.end(), bucket.begin(), bucket.end());
    const std::uint64_t roundPages = kReadRoundBytes / kTitlePageBytes;
    if (candidates.size() > roundPages) {
        snapshot.waiting = candidates.size() - roundPages;
        candidates.resize(roundPages);
    }
    const auto begin = std::chrono::steady_clock::now();
    const std::uint64_t roomPages = ReadOwnRoom() / kTitlePageBytes + video.freed.size() + video.freedBehindFence.size();
    if (roomPages < candidates.size() && !video.readStopped) {
        std::size_t evicted = 0;
        for (const std::uint64_t title : unused) {
            if (roomPages + evicted >= candidates.size()) break;
            DropReadPage(title, PageReturn::Unused, title, 0);
            ++evicted;
        }
    }
    if (!candidates.empty()) everMoved_.store(true, std::memory_order_release);
    std::vector<std::uint64_t> fresh;
    for (const std::uint64_t title : candidates) {
        auto range = ranges_.upper_bound(title);
        if (range != ranges_.begin() && !std::prev(range)->second.used && std::find(fresh.begin(), fresh.end(), std::prev(range)->first) == fresh.end()) {
            fresh.push_back(std::prev(range)->first);
        }
    }
    if (!fresh.empty() && !UseOwnRangesLater(fresh)) ++video.report.readOwnUnusedRounds;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const std::uint64_t title = candidates[index];
        auto range = ranges_.upper_bound(title);
        if (range == ranges_.begin() || !std::prev(range)->second.used) continue;
        if (ReadOwnRoom() < kTitlePageBytes) {
            snapshot.waiting += candidates.size() - index;
            break;
        }
        MoveOwnReadPage(title);
    }
    video.ownSnapshot = snapshot;
    video.report.readOwnCopyNanoseconds += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
    return snapshot.waiting != 0 && ReadOwnRoom() >= kTitlePageBytes;
}

bool TitleAddressSpace::PumpVideoPages() {
    const bool more = PumpReadPages();
    const bool moreOwn = PumpOwnReadPages();
    return PumpWritablePages() || more || moreOwn;
}

bool TitleAddressSpace::PumpWritablePages() {
    std::vector<Candidate> candidates;
    std::vector<std::uint64_t> leaving;
    std::vector<SettledBuffer> settled;
    const auto markSettled = [&] {
        for (const SettledBuffer& buffer : settled) {
            const auto live = video_->byId.find(buffer.id);
            if (live == video_->byId.end()) continue;
            live->second->second.complete = true;
            video_->report.sharedPages += buffer.shared;
            video_->report.pinnedPages += buffer.pinned;
        }
    };
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (!video_ || video_->setup.capBytes == 0 || std::chrono::steady_clock::now() < video_->nextPump) return false;
        CollectVideoWork(candidates, leaving, settled);
        if (candidates.empty() && leaving.empty()) {
            markSettled();
            return false;
        }
    }
    const bool more = candidates.size() * kTitlePageBytes >= kMoveRoundBytes;
    everMoved_.store(true, std::memory_order_release);
    Doing doing("the mover's round for pages the GPU may write", 0);
    doing.Stage("waits for the queue lock");
    std::lock_guard<std::mutex> queue(device_.QueueLock());
    doing.Stage("holds the queue lock and waits for an idle GPU (250 ms at most)");
    const auto begin = std::chrono::steady_clock::now();
    const bool idle = WaitForQueue(kMoveWaitNanoseconds);
    std::unique_lock<std::mutex> guard(lock_, std::try_to_lock);
    if (!guard.owns_lock()) return false;
    doing.Stage("holds the queue and address space locks");
    VideoPages& video = *video_;
    const auto waited = std::chrono::steady_clock::now();
    video.report.waitNanoseconds += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(waited - begin).count());
    if (!idle) {
        ++video.report.busyRounds;
        video.nextPump = waited + kBusyPause;
        return false;
    }
    if (!leaving.empty()) {
        ReturnPagesHeld(leaving, PageReturn::BufferReleased, true);
        ++video.report.returnEvents[static_cast<std::size_t>(PageReturn::BufferReleased)];
    }
    std::map<std::uint64_t, std::uint64_t> movedByGroup;
    const bool usable = UseOwnRanges(candidates);
    if (!usable) ++video.report.unusedRounds;
    for (const Candidate& candidate : candidates) {
        if (candidate.own && !usable) {
            settled.erase(std::remove_if(settled.begin(), settled.end(), [&](const SettledBuffer& buffer) { return buffer.id == candidate.group; }), settled.end());
            continue;
        }
        if (!Movable(candidate)) continue;
        std::uint32_t block = 0;
        std::uint32_t slot = 0;
        if (!TakeVideoPage(block, slot)) {
            ++video.report.overCapPages;
            settled.erase(std::remove_if(settled.begin(), settled.end(), [&](const SettledBuffer& buffer) { return buffer.id == candidate.group; }), settled.end());
            continue;
        }
        MovedPage moved;
        moved.title = candidate.title;
        moved.block = block;
        moved.page = slot;
        moved.group = candidate.group;
        moved.own = candidate.own;
        void* const held = static_cast<std::uint8_t*>(video.blocks[block].allocation.mapped) + std::uint64_t{slot} * kTitlePageBytes;
        std::uint32_t views = 1;
        if (candidate.own) {
            moved.views[0] = candidate.title;
            if (!GuardOwnPage(candidate.title, held, moved.protection)) views = kVideoPageGuardRefused;
        } else {
            views = video.setup.guard(candidate.source, 1, moved.views, kVideoPageViews);
        }
        if (views == kVideoPageGuardRefused || views > kVideoPageViews) {
            video.blocks[block].free.push_back(slot);
            video.pinned.insert(candidate.source);
            ++video.report.refusedPages;
            continue;
        }
        moved.viewCount = views;
        if (!candidate.own) {
            std::memcpy(held, reinterpret_cast<const void*>(candidate.source), kTitlePageBytes);
            const auto [first, last] = video.titlesOf.equal_range(candidate.source);
            for (auto it = first; it != last; ++it) {
                if (it->second != candidate.title && moved.extraCount < kVideoPageTitles - 1) moved.extra[moved.extraCount++] = it->second;
            }
        }
        const VkDeviceAddress device = video.blocks[block].allocation.address + std::uint64_t{slot} * kTitlePageBytes;
        video.moved[candidate.source] = moved;
        video.movedTitles[candidate.title] = device;
        for (std::uint32_t index = 0; index < moved.extraCount; ++index) video.movedTitles[moved.extra[index]] = device;
        for (std::uint32_t index = 0; index < views; ++index) {
            video.guardedViews[moved.views[index]] = candidate.source;
            video.everGuarded.insert(moved.views[index]);
        }
        SetEntry(candidate.title, device);
        for (std::uint32_t index = 0; index < moved.extraCount; ++index) SetEntry(moved.extra[index], device);
        video.heldBytes += kTitlePageBytes;
        ++video.report.movedPages;
        if (candidate.own) {
            video.heldOwnBytes += kTitlePageBytes;
            ++video.report.movedOwnPages;
        } else if (moved.extraCount != 0) {
            ++video.report.movedSharedPages;
        }
        if (candidate.partial) ++video.report.movedPartialPages;
        ++movedByGroup[candidate.group];
    }
    video.report.copyNanoseconds += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waited).count());
    markSettled();
    for (const auto& [group, pages] : movedByGroup) {
        VideoPageEvent event;
        event.moved = true;
        event.pages = pages;
        const auto live = video.byId.find(group);
        if (live != video.byId.end()) {
            event.buffer = live->second->first;
            event.bufferBytes = live->second->second.bytes;
        }
        AddEvent(event);
    }
    return more;
}

VideoPageReport TitleAddressSpace::TakeVideoPageReport() {
    std::lock_guard<std::mutex> guard(lock_);
    if (!video_) return {};
    VideoPages& video = *video_;
    VideoPageReport report = std::move(video.report);
    video.report = VideoPageReport{};
    report.capBytes = video.setup.capBytes;
    report.heldBytes = video.heldBytes - video.heldReadBytes;
    report.heldOwnBytes = video.heldOwnBytes;
    report.arenaBytes = video.arenaBytes;
    report.readCapBytes = video.setup.readCapBytes;
    report.readHeldBytes = video.heldReadBytes - video.heldOwnReadBytes;
    report.readKnownPages = video.gpuMapped.size();
    report.readWaitingPages = video.readWaiting;
    report.readStopped = video.readStopped;
    report.readOwnCapBytes = video.setup.readOwnCapBytes;
    report.readOwnHeldBytes = video.heldOwnReadBytes;
    report.readOwnUsedPages = video.ownSnapshot.used;
    report.readOwnUsedHeldPages = video.ownSnapshot.usedHeld;
    report.readOwnRootWrittenPages = video.ownSnapshot.rootWritten;
    report.readOwnLookedAt = video.ownSnapshot.lookedAt;
    report.readOwnPinnedPages = video.ownSnapshot.pinned;
    report.readOwnWritablePages = video.ownSnapshot.writable;
    report.readOwnSettlingPages = video.ownSnapshot.settling;
    report.readOwnWaitingPages = video.ownSnapshot.waiting;
    report.buffers = video.buffers.size();
    for (const auto& [address, buffer] : video.buffers) report.buffersHome += buffer.home ? 1 : 0;
    return report;
}

void TitleAddressSpace::ReturnSource(std::uint64_t source) {
    Doing doing("a physical page is handed back before a streamed write or a new mapping", source);
    std::lock_guard<std::mutex> guard(lock_);
    doing.Stage(kHoldsLock);
    if (!video_) return;
    const std::uint64_t page = source & ~(kTitlePageBytes - 1);
    if (video_->moved.count(page) != 0) {
        ReturnForUse(page, PageReturn::GivenOut, source, 0);
        return;
    }
    video_->setup.guard(page, 0, nullptr, 0);
}

bool TitleAddressSpace::ReturnForAccess(std::uint64_t address, std::uint64_t code, bool write) {
    if (!everMoved_.load(std::memory_order_acquire)) return false;
    Doing doing("a CPU access to a guarded page", address);
    std::lock_guard<std::mutex> guard(lock_);
    doing.Stage(kHoldsLock);
    if (!video_) return false;
    const std::uint64_t view = address & ~(kTitlePageBytes - 1);
    const auto found = video_->guardedViews.find(view);
    if (found != video_->guardedViews.end()) {
        const std::uint64_t source = found->second;
        if (video_->moved.count(source) != 0) {
            ReturnForUse(source, PageReturn::CpuAccess, address, code);
            return true;
        }
        video_->guardedViews.erase(found);
    }
    return video_->everGuarded.count(view) != 0 && Reachable(address, write);
}

bool TitleAddressSpace::ReturnForSystemCall(std::uint64_t address, std::uint64_t bytes, bool write, std::uint64_t code) {
    if (bytes == 0 || address + bytes < address || !everMoved_.load(std::memory_order_acquire)) return false;
    Doing doing("a system call that was refused a buffer over guarded pages", address);
    std::lock_guard<std::mutex> guard(lock_);
    doing.Stage(kHoldsLock);
    if (!video_) return false;
    const std::uint64_t first = address & ~(kTitlePageBytes - 1);
    const std::uint64_t count = (address + bytes - first + kTitlePageBytes - 1) / kTitlePageBytes;
    std::uint64_t handedBack = 0;
    for (std::uint64_t index = 0; index < count; ++index) {
        const std::uint64_t view = first + index * kTitlePageBytes;
        const std::uint64_t at = (std::max)(view, address);
        const auto found = video_->guardedViews.find(view);
        const auto moved = found != video_->guardedViews.end() ? video_->moved.find(found->second) : video_->moved.end();
        if (moved != video_->moved.end()) {
            if (moved->second.readOnly && !write) continue;
            ReturnForUse(moved->first, PageReturn::SystemCall, at, code);
            ++handedBack;
        } else if (video_->everGuarded.count(view) != 0 && Reachable(at, write)) {
            ++handedBack;
        }
    }
    if (handedBack == 0) return false;
    g_systemCalls.fetch_add(1, std::memory_order_relaxed);
    g_systemCallViews.fetch_add(handedBack, std::memory_order_relaxed);
    return true;
}

std::uint64_t TitleAddressSpace::RepeatedAccesses(std::uint64_t& address, std::uint64_t& code, std::uint32_t& thread) {
    address = g_accesses.mostAddress.load(std::memory_order_relaxed);
    code = g_accesses.mostCode.load(std::memory_order_relaxed);
    thread = g_accesses.mostThread.load(std::memory_order_relaxed);
    return g_accesses.mostRepeats.load(std::memory_order_relaxed);
}

std::string TitleAddressSpace::CompareReadCopies(std::uint32_t lockProbeMilliseconds, std::uint32_t limitMilliseconds) const {
    constexpr std::size_t kPlacesNamed = 6;
    const auto take = [&](std::unique_lock<std::mutex>& guard) {
        for (std::uint32_t waited = 0; !guard.owns_lock() && waited < lockProbeMilliseconds; waited += 10) {
            Sleep(10);
            if (guard.try_lock()) break;
        }
        return guard.owns_lock();
    };
    std::vector<std::uint64_t> sources;
    {
        std::unique_lock<std::mutex> guard(lock_, std::try_to_lock);
        if (!take(guard) || !video_) return {};
        for (const auto& [source, page] : video_->moved) {
            if (page.readOnly) sources.push_back(source);
        }
    }
    if (sources.empty()) return {};
    const std::uint64_t begin = GetTickCount64();
    std::vector<std::uint8_t> copy(kTitlePageBytes);
    std::size_t compared = 0;
    std::size_t differed = 0;
    std::size_t gone = 0;
    std::string places;
    char text[640];
    for (const std::uint64_t source : sources) {
        if (GetTickCount64() - begin > limitMilliseconds) break;
        std::unique_lock<std::mutex> guard(lock_, std::try_to_lock);
        if (!take(guard)) break;
        const auto found = video_->moved.find(source);
        if (found == video_->moved.end() || !found->second.readOnly) {
            ++gone;
            continue;
        }
        const MovedPage& page = found->second;
        std::memcpy(copy.data(), static_cast<const std::uint8_t*>(video_->blocks[page.block].allocation.mapped) + std::uint64_t{page.page} * kTitlePageBytes, kTitlePageBytes);
        ++compared;
        if (std::memcmp(copy.data(), reinterpret_cast<const void*>(source), kTitlePageBytes) == 0) continue;
        if (differed++ < kPlacesNamed) {
            std::snprintf(text, sizeof(text), " 0x%llx", static_cast<unsigned long long>(page.title));
            places += text;
        }
    }
    std::snprintf(text, sizeof(text),
                  "%zu of %zu read-only copies in video memory were compared with system memory in %llu ms (%u ms at most; %zu were dropped meanwhile): %zu differ%s%s. A "
                  "copy that differs was written by the GPU although no buffer or view said it may, or system memory was written without a fault (a system call that had "
                  "the page locked before it was write-protected); the CPU then reads other bytes than shaders do",
                  compared, sources.size(), static_cast<unsigned long long>(GetTickCount64() - begin), limitMilliseconds, gone, differed,
                  differed != 0 ? ", the first at GPU address(es)" : "", places.c_str());
    return text;
}

std::vector<std::string> TitleAddressSpace::DescribeActivity(std::uint32_t lockProbeMilliseconds, std::uint32_t compareMilliseconds) const {
    std::vector<std::string> lines;
    char text[640];
    const std::uint64_t now = GetTickCount64();
    const std::uint64_t resumed = g_accesses.resumed.load(std::memory_order_relaxed);
    const std::uint64_t lastTick = g_accesses.lastTick.load(std::memory_order_relaxed);
    std::snprintf(text, sizeof(text),
                  "guarded title pages: %llu CPU access(es) handed a page back and were resumed, %llu access violation(s) were not about a guarded page and went on to the "
                  "next handler; the last resumed one was %llu ms ago at 0x%llx by code 0x%llx on thread %lu and is number %llu in a row of that same access; the longest "
                  "such row is %llu (at 0x%llx by code 0x%llx on thread %lu; a row that keeps growing is an access that faults again after every resume)",
                  static_cast<unsigned long long>(resumed), static_cast<unsigned long long>(g_accesses.passedOn.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(resumed != 0 ? now - lastTick : 0), static_cast<unsigned long long>(g_accesses.lastAddress.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_accesses.lastCode.load(std::memory_order_relaxed)),
                  static_cast<unsigned long>(g_accesses.lastThread.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_accesses.repeats.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_accesses.mostRepeats.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_accesses.mostAddress.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_accesses.mostCode.load(std::memory_order_relaxed)),
                  static_cast<unsigned long>(g_accesses.mostThread.load(std::memory_order_relaxed)));
    lines.emplace_back(text);
    std::snprintf(text, sizeof(text),
                  "guarded title pages: %llu system call(s) of the title that failed over a guarded page (ReadFile into it, WriteFile from it) had %llu view(s) of 64 KiB "
                  "under their buffer handed back, or found open again, and were repeated",
                  static_cast<unsigned long long>(g_systemCalls.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_systemCallViews.load(std::memory_order_relaxed)));
    lines.emplace_back(text);
    std::size_t listed = 0;
    for (const Activity& slot : g_activities) {
        const std::uint32_t thread = slot.thread.load(std::memory_order_acquire);
        const char* const what = slot.what.load(std::memory_order_relaxed);
        const char* const stage = slot.stage.load(std::memory_order_relaxed);
        if (thread == 0 || what == nullptr) continue;
        ++listed;
        std::snprintf(text, sizeof(text), "address space operation in flight on thread %lu for %llu ms: %s (at 0x%llx); it %s", static_cast<unsigned long>(thread),
                      static_cast<unsigned long long>(now - slot.since.load(std::memory_order_relaxed)), what,
                      static_cast<unsigned long long>(slot.address.load(std::memory_order_relaxed)), stage != nullptr ? stage : "has no stage yet");
        lines.emplace_back(text);
    }
    std::snprintf(text, sizeof(text), "%zu address space operation(s) in flight (%llu more were never listed because all %zu places were taken)", listed,
                  static_cast<unsigned long long>(g_activitiesUnlisted.load(std::memory_order_relaxed)), kActivities);
    lines.emplace_back(text);
    std::unique_lock<std::mutex> guard(lock_, std::try_to_lock);
    for (std::uint32_t waited = 0; !guard.owns_lock() && waited < lockProbeMilliseconds; waited += 10) {
        Sleep(10);
        if (guard.try_lock()) break;
    }
    if (!guard.owns_lock()) {
        std::snprintf(text, sizeof(text), "the address space lock could not be taken once in %u ms: a thread holds it (see the operations in flight and the thread stacks)",
                      lockProbeMilliseconds);
        lines.emplace_back(text);
        return lines;
    }
    if (!video_) {
        lines.emplace_back("the address space lock is free; no title page is kept in video memory (both settings off)");
        return lines;
    }
    const VideoPages& video = *video_;
    std::uint64_t readOnly = 0;
    for (const auto& [source, page] : video.moved) readOnly += page.readOnly ? 1 : 0;
    std::snprintf(text, sizeof(text),
                  "the address space lock is free; %zu page(s) are in video memory (%llu of them read-only copies), %zu CPU view(s) are guarded, %zu dropped copy slot(s) wait "
                  "for a batch to pass and %zu more wait behind a fence that is %s, %zu fence(s) of GPU waits that timed out are still open, %zu physical page(s) are pinned, "
                  "%zu read-only candidate(s) are waiting for their time",
                  video.moved.size(), static_cast<unsigned long long>(readOnly), video.guardedViews.size(), video.freed.size(), video.freedBehindFence.size(),
                  video.freedFence == VK_NULL_HANDLE ? "not there" : (device_.Vk().getFenceStatus(device_.Handle(), video.freedFence) == VK_SUCCESS ? "signaled" : "not signaled yet"),
                  video.lateFences.size(), video.pinned.size(), video.readDue.size());
    lines.emplace_back(text);
    guard.unlock();
    std::string compared = CompareReadCopies(lockProbeMilliseconds, compareMilliseconds);
    if (!compared.empty()) lines.push_back(std::move(compared));
    return lines;
}

}
