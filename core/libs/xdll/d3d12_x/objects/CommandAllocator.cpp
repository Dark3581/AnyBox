#include "CommandAllocator.hpp"

#include "AbxApi.hpp"
#include "LiveStats.hpp"

#include <algorithm>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <optional>
#include <utility>

namespace abx::d3d12x {

HRESULT CommandAllocator::Create(const VulkanDevice& device, IUnknown* parent, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** allocator, std::string& error) {
    if (allocator == nullptr) {
        error = "no output for the command allocator";
        return E_POINTER;
    }
    *allocator = nullptr;
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE && type != D3D12_COMMAND_LIST_TYPE_COPY) {
        error = "command list type " + std::to_string(static_cast<int>(type)) + " is not direct, compute or copy";
        return E_INVALIDARG;
    }
    CommandAllocator* created = new CommandAllocator(device, parent, type);
    VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    info.queueFamilyIndex = device.QueueFamily();
    if (device.Vk().createCommandPool(device.Handle(), &info, nullptr, &created->pool_) != VK_SUCCESS) {
        created->pool_ = VK_NULL_HANDLE;
        created->Release();
        error = "vkCreateCommandPool failed";
        return E_OUTOFMEMORY;
    }
    const HRESULT result = created->QueryInterface(riid, allocator);
    created->Release();
    if (FAILED(result)) error = "the command allocator does not implement the requested interface";
    return result;
}

CommandAllocator::~CommandAllocator() {
    for (const TransientBlock& block : transient_) VirtualFree(reinterpret_cast<void*>(block.base), 0, MEM_RELEASE);
    ReturnScratch();
    if (pool_ != VK_NULL_HANDLE) device_.Vk().destroyCommandPool(device_.Handle(), pool_, nullptr);
}

bool CommandAllocator::Implements(REFIID riid) const {
    return DeviceChildBase::Implements(riid) || riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12CommandAllocator);
}

HRESULT STDMETHODCALLTYPE CommandAllocator::Reset() {
    if (device_.Vk().resetCommandPool(device_.Handle(), pool_, 0) != VK_SUCCESS) {
        return E_FAIL;
    }
    next_ = 0;
    ReturnScratch();
    transientNext_ = 0;
    for (TransientBlock& block : transient_) block.used = 0;
    return S_OK;
}

std::uintptr_t CommandAllocator::Transient(std::size_t bytes, std::size_t alignment, TransientBlock*& created, std::string& error) {
    created = nullptr;
    if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0 || alignment > 65536) {
        error = "a transient allocation of " + std::to_string(bytes) + " bytes with alignment " + std::to_string(alignment);
        return 0;
    }
    for (; transientNext_ < transient_.size(); ++transientNext_) {
        TransientBlock& block = transient_[transientNext_];
        const std::size_t start = (block.used + alignment - 1) & ~(alignment - 1);
        if (start <= block.size && bytes <= block.size - start) {
            block.used = start + bytes;
            return block.base + start;
        }
    }
    constexpr std::size_t kBlockBytes = 4u << 20;
    const std::size_t size = (std::max)(kBlockBytes, (bytes + 0xFFFFu) & ~std::size_t{0xFFFF});
    const std::uintptr_t base = ReserveHigh(size, error);
    if (base == 0) return 0;
    transient_.insert(transient_.begin() + static_cast<std::ptrdiff_t>(transientNext_), TransientBlock{base, size, bytes});
    created = &transient_[transientNext_];
    return created->base;
}

std::uintptr_t CommandAllocator::ReserveHigh(std::size_t size, std::string& error) {
    using VirtualAlloc2Function = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    static const VirtualAlloc2Function virtualAlloc2 = [] {
        const HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
        return kernelbase != nullptr ? reinterpret_cast<VirtualAlloc2Function>(reinterpret_cast<void*>(GetProcAddress(kernelbase, "VirtualAlloc2"))) : nullptr;
    }();
    if (virtualAlloc2 == nullptr) {
        error = "VirtualAlloc2 is not available";
        return 0;
    }
    MEM_ADDRESS_REQUIREMENTS requirements{};
    requirements.LowestStartingAddress = reinterpret_cast<void*>(std::uintptr_t{1} << 38);
    requirements.HighestEndingAddress = reinterpret_cast<void*>((std::uintptr_t{1} << 40) - 1);
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    void* const base = virtualAlloc2(GetCurrentProcess(), nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &parameter, 1);
    if (base == nullptr) {
        error = "no address range between 256 GiB and 1 TiB for " + std::to_string(size) + " bytes (error " + std::to_string(GetLastError()) + ")";
        return 0;
    }
    return reinterpret_cast<std::uintptr_t>(base);
}

namespace {

std::mutex g_scratchLock;

}

void CommandAllocator::ReturnScratch() {
    if (scratch_.empty()) return;
    const std::lock_guard guard(g_scratchLock);
    std::vector<ScratchBuffer>& free = FreeScratch();
    free.insert(free.end(), scratch_.begin(), scratch_.end());
    scratch_.clear();
}

std::vector<CommandAllocator::ScratchBuffer>& CommandAllocator::FreeScratch() {
    static std::vector<ScratchBuffer>* free = new std::vector<ScratchBuffer>();
    return *free;
}

bool CommandAllocator::Scratch(VkDeviceSize bytes, VkBuffer& buffer, std::string& error) {
    {
        const std::lock_guard guard(g_scratchLock);
        std::vector<ScratchBuffer>& free = FreeScratch();
        std::size_t best = free.size();
        for (std::size_t index = 0; index < free.size(); ++index) {
            if (free[index].size < bytes || free[index].size > bytes * 4) continue;
            if (best == free.size() || free[index].size < free[best].size) best = index;
        }
        if (best != free.size()) {
            scratch_.push_back(free[best]);
            free.erase(free.begin() + static_cast<std::ptrdiff_t>(best));
            buffer = scratch_.back().buffer;
            return true;
        }
    }
    const auto& vk = device_.Vk();
    ScratchBuffer made;
    made.size = bytes;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vk.createBuffer(device_.Handle(), &info, nullptr, &made.buffer) != VK_SUCCESS) {
        error = "vkCreateBuffer failed for a copy scratch buffer";
        return false;
    }
    VkMemoryRequirements needs{};
    vk.getBufferMemoryRequirements(device_.Handle(), made.buffer, &needs);
    const std::optional<std::uint32_t> local = device_.MemoryType(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const std::optional<std::uint32_t> system = device_.SystemMemoryType(needs.memoryTypeBits);
    VkMemoryPriorityAllocateInfoEXT priority{VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT};
    priority.priority = 0.75f;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = needs.size;
    bool inSystemMemory = false;
    bool allocated = false;
    if (local) {
        if (device_.PageableMemory()) allocate.pNext = &priority;
        allocate.memoryTypeIndex = *local;
        allocated = vk.allocateMemory(device_.Handle(), &allocate, nullptr, &made.memory) == VK_SUCCESS;
        allocate.pNext = nullptr;
    }
    if (!allocated && system) {
        allocate.memoryTypeIndex = *system;
        allocated = vk.allocateMemory(device_.Handle(), &allocate, nullptr, &made.memory) == VK_SUCCESS;
        inSystemMemory = allocated;
    }
    if (!allocated || vk.bindBufferMemory(device_.Handle(), made.buffer, made.memory, 0) != VK_SUCCESS) {
        if (allocated) vk.freeMemory(device_.Handle(), made.memory, nullptr);
        vk.destroyBuffer(device_.Handle(), made.buffer, nullptr);
        error = "no video or system memory for a copy scratch buffer of " + std::to_string(bytes) + " bytes";
        return false;
    }
    Stats().scratchBytes += needs.size;
    ++Stats().scratchBuffers;
    if (inSystemMemory) {
        Stats().scratchSystemBytes += needs.size;
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) AbxLog("d3d12_x", "video memory is exhausted: copy scratch buffers are placed in system memory (first: %llu KiB)", static_cast<unsigned long long>(needs.size >> 10));
    }
    scratch_.push_back(made);
    buffer = made.buffer;
    return true;
}

VkCommandBuffer CommandAllocator::Next(std::string& error) {
    if (next_ < buffers_.size()) {
        return buffers_[next_++];
    }
    VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    info.commandPool = pool_;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    VkCommandBuffer buffer = VK_NULL_HANDLE;
    if (device_.Vk().allocateCommandBuffers(device_.Handle(), &info, &buffer) != VK_SUCCESS) {
        error = "vkAllocateCommandBuffers failed";
        return VK_NULL_HANDLE;
    }
    buffers_.push_back(buffer);
    next_ = buffers_.size();
    return buffer;
}

}
