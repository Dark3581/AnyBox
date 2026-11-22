#include "TitleBufferWindow.hpp"

#include <algorithm>

namespace abx::d3d12x {

namespace {

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

}

std::optional<std::uint64_t> FindFreeRange(std::uint64_t bytes, std::uint64_t alignment, std::uint64_t start, std::uint64_t end) {
    if (bytes == 0 || alignment == 0 || start >= end) {
        return std::nullopt;
    }
    std::uint64_t cursor = AlignUp(start, alignment);
    while (cursor < end && end - cursor >= bytes) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) != sizeof(info)) {
            return std::nullopt;
        }
        const std::uint64_t regionStart = reinterpret_cast<std::uint64_t>(info.BaseAddress);
        const std::uint64_t regionEnd = regionStart + info.RegionSize;
        if (info.State == MEM_FREE && (std::min)(regionEnd, end) - cursor >= bytes) {
            return cursor;
        }
        cursor = AlignUp(regionEnd, alignment);
    }
    return std::nullopt;
}

std::unique_ptr<TitleBufferWindow> TitleBufferWindow::Create(const VulkanDevice& device, std::uint64_t bytes, std::string& error) {
    std::unique_ptr<TitleBufferWindow> window(new TitleBufferWindow(device));
    if (!window->Initialize(bytes, error)) {
        return nullptr;
    }
    return window;
}

TitleBufferWindow::~TitleBufferWindow() {
    const DeviceFunctions& vk = device_.Vk();
    if (buffer_ != VK_NULL_HANDLE) {
        vk.destroyBuffer(device_.Handle(), buffer_, nullptr);
    }
    if (memory_ != VK_NULL_HANDLE) {
        vk.freeMemory(device_.Handle(), memory_, nullptr);
    }
    if (base_ != nullptr) {
        VirtualFree(base_, 0, MEM_RELEASE);
    }
}

bool TitleBufferWindow::Contains(std::uint64_t address, std::uint64_t bytes) const {
    return address >= Start() && bytes <= size_ && address - Start() <= size_ - bytes;
}

bool TitleBufferWindow::Initialize(std::uint64_t bytes, std::string& error) {
    if (device_.HostPointerProperties() == nullptr || !device_.BufferDeviceAddress()) {
        error = "the device was created without host memory import or buffer device addresses";
        return false;
    }
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const std::uint64_t alignment = std::max<std::uint64_t>(system.dwAllocationGranularity, device_.HostImportAlignment());
    const std::uint64_t size = AlignUp(bytes, std::max<std::uint64_t>(system.dwPageSize, device_.HostImportAlignment()));
    if (size == 0 || size > kLowWindowEnd - kLowWindowStart) {
        error = "the window does not fit below 4 GB";
        return false;
    }
    const std::optional<std::uint64_t> start = FindFreeRange(size, alignment, kLowWindowStart, kLowWindowEnd);
    if (!start) {
        error = "no free range of " + std::to_string(size) + " bytes below 4 GB";
        return false;
    }
    base_ = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(*start), static_cast<SIZE_T>(size), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (base_ == nullptr) {
        error = "VirtualAlloc below 4 GB failed with " + std::to_string(GetLastError());
        return false;
    }
    size_ = size;
    const DeviceFunctions& vk = device_.Vk();
    VkMemoryHostPointerPropertiesEXT hostProperties{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (device_.HostPointerProperties()(device_.Handle(), VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, base_, &hostProperties) != VK_SUCCESS) {
        error = "vkGetMemoryHostPointerPropertiesEXT failed";
        return false;
    }
    VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
    bufferInfo.size = size;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vk.createBuffer(device_.Handle(), &bufferInfo, nullptr, &buffer_) != VK_SUCCESS) {
        buffer_ = VK_NULL_HANDLE;
        error = "vkCreateBuffer failed for the window";
        return false;
    }
    VkMemoryRequirements requirements{};
    vk.getBufferMemoryRequirements(device_.Handle(), buffer_, &requirements);
    const std::uint32_t typeBits = requirements.memoryTypeBits & hostProperties.memoryTypeBits;
    std::optional<std::uint32_t> type = device_.MemoryType(typeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!type) {
        error = "no host-coherent memory type can import the window";
        return false;
    }
    VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    import.pHostPointer = base_;
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &import};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
    allocate.allocationSize = size;
    allocate.memoryTypeIndex = *type;
    const VkResult imported = vk.allocateMemory(device_.Handle(), &allocate, nullptr, &memory_);
    if (imported != VK_SUCCESS) {
        memory_ = VK_NULL_HANDLE;
        error = "importing the window failed with " + std::to_string(static_cast<int>(imported));
        return false;
    }
    if (vk.bindBufferMemory(device_.Handle(), buffer_, memory_, 0) != VK_SUCCESS) {
        error = "vkBindBufferMemory failed for the window";
        return false;
    }
    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = buffer_;
    address_ = vk.getBufferDeviceAddress(device_.Handle(), &addressInfo);
    if (address_ == 0) {
        error = "the window has no device address";
        return false;
    }
    return true;
}

}
