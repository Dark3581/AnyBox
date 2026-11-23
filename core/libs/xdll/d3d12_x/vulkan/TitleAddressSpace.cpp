#include "TitleAddressSpace.hpp"

#include "TitleBufferWindow.hpp"

#include <algorithm>
#include <cstring>

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

}

std::unique_ptr<TitleAddressSpace> TitleAddressSpace::Create(const VulkanDevice& device, std::string& error) {
    std::unique_ptr<TitleAddressSpace> space(new TitleAddressSpace(device));
    if (!space->Initialize(error)) return nullptr;
    return space;
}

TitleAddressSpace::~TitleAddressSpace() {
    for (auto& [start, range] : ranges_) Destroy(range.imported);
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
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
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
    if (!CreateTable(kTitlePageBytes, nullPage_, error) || !CreateTable(kTitlePagesPerRegion * sizeof(std::uint64_t), nullRegion_, error) ||
        !CreateTable(kTitleRegionCount * sizeof(std::uint64_t), root_, error)) {
        return false;
    }
    std::memset(nullPage_.mapped, 0, kTitlePageBytes);
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
        table[page % kTitlePagesPerRegion] = device + page * kTitlePageBytes - deviceStart;
    }
}

void TitleAddressSpace::Clear(std::uint64_t start, std::uint64_t bytes) {
    for (std::uint64_t page = start / kTitlePageBytes; page <= (start + bytes - 1) / kTitlePageBytes; ++page) {
        Allocation& table = regions_[(page / kTitlePagesPerRegion) % kTitleRegionCount];
        if (table.buffer != VK_NULL_HANDLE) static_cast<std::uint64_t*>(table.mapped)[page % kTitlePagesPerRegion] = nullPage_.address;
    }
}

bool TitleAddressSpace::ImportLocked(std::uint64_t start, std::uint64_t bytes, std::string& error) {
    const DeviceFunctions& vk = device_.Vk();
    Range range;
    range.bytes = bytes;
    VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
    bufferInfo.size = bytes;
    bufferInfo.usage = kImportUsage;
    Allocation& imported = range.imported;
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
    range.device = imported.address;
    range.buffer = imported.buffer;
    Point(start, bytes, range.device, start);
    importedBytes_ += bytes;
    ranges_[start] = range;
    return true;
}

bool TitleAddressSpace::Import(std::uint64_t start, std::uint64_t bytes, std::string& error) {
    if (bytes == 0) return true;
    const std::uint64_t alignment = (std::max<std::uint64_t>)(device_.HostImportAlignment(), 4096);
    std::uint64_t first = AlignDown(start, alignment);
    std::uint64_t last = AlignUp(start + bytes, alignment);
    std::lock_guard<std::mutex> guard(lock_);
    for (const auto& [rangeStart, range] : ranges_) {
        if (rangeStart <= first && rangeStart + range.bytes >= last) return true;
    }
    bool overlapped = false;
    for (auto it = ranges_.begin(); it != ranges_.end();) {
        const std::uint64_t rangeEnd = it->first + it->second.bytes;
        if (it->first < last && rangeEnd > first && it->second.imported.memory != VK_NULL_HANDLE) {
            if (!overlapped) {
                std::lock_guard<std::mutex> queue(device_.QueueLock());
                device_.Vk().queueWaitIdle(device_.Queue());
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
    std::lock_guard<std::mutex> guard(lock_);
    std::vector<std::pair<std::uint64_t, Range>> released;
    for (auto it = ranges_.begin(); it != ranges_.end();) {
        if (it->first < end && it->first + it->second.bytes > start) {
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
        std::lock_guard<std::mutex> queue(device_.QueueLock());
        device_.Vk().queueWaitIdle(device_.Queue());
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
    for (const auto& [rangeStart, range] : ranges_) {
        if (rangeStart < AlignUp(end, kTitlePageBytes) && rangeStart + range.bytes > AlignDown(start, kTitlePageBytes)) Point(rangeStart, range.bytes, range.device, rangeStart);
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
    std::lock_guard<std::mutex> guard(lock_);
    auto it = ranges_.upper_bound(address);
    if (it == ranges_.begin()) return std::nullopt;
    --it;
    const std::uint64_t offset = address - it->first;
    if (offset > it->second.bytes || bytes > it->second.bytes - offset || it->second.buffer == VK_NULL_HANDLE) return std::nullopt;
    return Location{it->second.buffer, it->second.bufferOffset + offset};
}

std::uint64_t TitleAddressSpace::ImportedBytes() const {
    std::lock_guard<std::mutex> guard(lock_);
    return importedBytes_;
}

}
