#include "Resource.hpp"

#include "AbxApi.hpp"
#include "DescriptorWriter.hpp"
#include "LiveStats.hpp"
#include "ResourceState.hpp"
#include "GraphicsState.hpp"
#include "TextureDesc.hpp"
#include "TextureReadback.hpp"

#include <algorithm>
#include <iterator>
#include <vector>
#include <memory>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <map>
#include <mutex>

namespace abx::d3d12x {

namespace {

constexpr VkDeviceSize kPoolBlockBytes = 64ull << 20;
constexpr VkDeviceSize kPooledLimit = 1ull << 20;

struct PoolBlock {
    const VulkanDevice* device = nullptr;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::uint32_t type = 0;
    std::map<VkDeviceSize, VkDeviceSize> free;
};

struct TexturePool {
    std::mutex lock;
    std::vector<std::unique_ptr<PoolBlock>> blocks;
};

TexturePool& Pool() {
    static TexturePool* pool = new TexturePool();
    return *pool;
}

bool Carve(PoolBlock& block, VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset) {
    for (auto range = block.free.begin(); range != block.free.end(); ++range) {
        const VkDeviceSize start = range->first;
        const VkDeviceSize length = range->second;
        const VkDeviceSize aligned = (start + alignment - 1) / alignment * alignment;
        if (aligned + size > start + length) continue;
        block.free.erase(range);
        if (aligned > start) block.free[start] = aligned - start;
        if (aligned + size < start + length) block.free[aligned + size] = start + length - (aligned + size);
        offset = aligned;
        return true;
    }
    return false;
}

constexpr float kUsedMemoryPriority = 0.75f;

bool PriorityByUse() {
    static const bool on = [] {
        const char* value = AbxSetting("d3d12_x.memory_priority_by_use");
        return value == nullptr || std::strtoul(value, nullptr, 10) != 0;
    }();
    return on;
}

bool PoolAllocate(const VulkanDevice& device, std::uint32_t type, VkDeviceSize size, VkDeviceSize alignment, void*& handle, VkDeviceSize& offset) {
    TexturePool& pool = Pool();
    const std::lock_guard guard(pool.lock);
    for (const std::unique_ptr<PoolBlock>& block : pool.blocks) {
        if (block->type == type && Carve(*block, size, (std::max)(alignment, VkDeviceSize{1}), offset)) {
            handle = block.get();
            Stats().poolUsedBytes += size;
            return true;
        }
    }
    auto block = std::make_unique<PoolBlock>();
    VkMemoryPriorityAllocateInfoEXT priority{VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT};
    priority.priority = kUsedMemoryPriority;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    if (device.PageableMemory() && PriorityByUse()) allocate.pNext = &priority;
    allocate.allocationSize = kPoolBlockBytes;
    allocate.memoryTypeIndex = type;
    if (device.Vk().allocateMemory(device.Handle(), &allocate, nullptr, &block->memory) != VK_SUCCESS) return false;
    block->device = &device;
    block->type = type;
    block->free[0] = kPoolBlockBytes;
    Stats().poolBlockBytes += kPoolBlockBytes;
    if (!Carve(*block, size, (std::max)(alignment, VkDeviceSize{1}), offset)) return false;
    Stats().poolUsedBytes += size;
    handle = block.get();
    pool.blocks.push_back(std::move(block));
    if (pool.blocks.size() == 1) {
        AbxLog("d3d12_x", "textures up to %llu KiB share %llu MiB blocks of video memory instead of one allocation each (each allocation costs at least 64 KiB on this driver)",
               static_cast<unsigned long long>(kPooledLimit >> 10), static_cast<unsigned long long>(kPoolBlockBytes >> 20));
    }
    return true;
}

void PoolFree(void* handle, VkDeviceSize offset, VkDeviceSize size) {
    TexturePool& pool = Pool();
    const std::lock_guard guard(pool.lock);
    Stats().poolUsedBytes -= size;
    auto& free = static_cast<PoolBlock*>(handle)->free;
    VkDeviceSize start = offset;
    VkDeviceSize length = size;
    auto next = free.lower_bound(start);
    if (next != free.begin()) {
        auto previous = std::prev(next);
        if (previous->first + previous->second == start) {
            start = previous->first;
            length += previous->second;
            free.erase(previous);
        }
    }
    next = free.lower_bound(start + length);
    if (next != free.end() && next->first == start + length) {
        length += next->second;
        free.erase(next);
    }
    free[start] = length;
    PoolBlock* const emptied = static_cast<PoolBlock*>(handle);
    if (start != 0 || length != kPoolBlockBytes) return;
    const bool another = std::any_of(pool.blocks.begin(), pool.blocks.end(), [emptied](const std::unique_ptr<PoolBlock>& block) {
        return block.get() != emptied && block->type == emptied->type && block->free.size() == 1 && block->free.begin()->second == kPoolBlockBytes;
    });
    if (!another) return;
    emptied->device->Vk().freeMemory(emptied->device->Handle(), emptied->memory, nullptr);
    Stats().poolBlockBytes -= kPoolBlockBytes;
    pool.blocks.erase(std::find_if(pool.blocks.begin(), pool.blocks.end(), [emptied](const std::unique_ptr<PoolBlock>& block) { return block.get() == emptied; }));
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "an empty small-texture pool block is returned to the driver when another empty block of its memory type is kept (%llu MiB each)",
               static_cast<unsigned long long>(kPoolBlockBytes >> 20));
    }
}

}


namespace {

constexpr UINT kKnownHeapFlags = D3D12_HEAP_FLAG_DENY_BUFFERS | D3D12_HEAP_FLAG_ALLOW_DISPLAY | D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES | D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES |
                                 D3D12_HEAP_FLAG_HARDWARE_PROTECTED | D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH | D3D12_HEAP_FLAG_ALLOW_SHADER_ATOMICS | D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT |
                                 D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;

bool CheckHeap(const D3D12_HEAP_PROPERTIES& heap, D3D12_HEAP_FLAGS flags, std::string& error) {
    if (heap.Type != D3D12_HEAP_TYPE_DEFAULT && heap.Type != D3D12_HEAP_TYPE_UPLOAD && heap.Type != D3D12_HEAP_TYPE_READBACK) {
        error = "heap type " + std::to_string(static_cast<int>(heap.Type)) + " is not a default, upload or readback heap";
        return false;
    }
    if (heap.CreationNodeMask > 1 || heap.VisibleNodeMask > 1) {
        error = "the heap names a GPU node other than the first";
        return false;
    }
    const UINT bits = static_cast<UINT>(flags);
    if ((bits & ~kKnownHeapFlags) != 0 || (bits & (D3D12_HEAP_FLAG_HARDWARE_PROTECTED | D3D12_HEAP_FLAG_ALLOW_WRITE_WATCH)) != 0) {
        error = "heap flags " + std::to_string(bits) + " are not supported";
        return false;
    }
    return true;
}

}

HRESULT Resource::CreateCommitted(const ResourceContext& context, IUnknown* parent, const D3D12_HEAP_PROPERTIES& heap, D3D12_HEAP_FLAGS heapFlags,
                                  const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initial, REFIID riid, void** resource, std::string& error) {
    if (resource == nullptr) {
        error = "no output for the resource";
        return E_POINTER;
    }
    *resource = nullptr;
    if (!CheckHeap(heap, heapFlags, error)) {
        return E_INVALIDARG;
    }
    Resource* created = new Resource(context, parent, heap, heapFlags, desc);
    const bool built = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? created->CreateBuffer(error) : created->CreateTexture(initial, error);
    if (built && desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && desc.Width >= 1024 && (static_cast<UINT>(desc.Flags) & 7u) == 0) {
        AbxLog("d3d12_x", "DIAG big texture %p %llux%u mips %u format %d flags 0x%x heap %d", static_cast<void*>(created->texture_.image), static_cast<unsigned long long>(desc.Width),
               desc.Height, desc.MipLevels, static_cast<int>(desc.Format), static_cast<unsigned>(desc.Flags), static_cast<int>(heap.Type));
    }
    if (!built) {
        created->Release();
        return E_INVALIDARG;
    }
    const HRESULT result = created->QueryInterface(riid, resource);
    created->Release();
    if (FAILED(result)) error = "the resource does not implement the requested interface";
    return result;
}

namespace {

constexpr UINT kBufferFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                              D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
constexpr UINT kDesktopResourceFlags = 0xFFFF;

}

bool Resource::CheckBufferDesc(UINT allowedFlags, std::string& error) const {
    if (desc_.Width == 0 || desc_.Height != 1 || desc_.DepthOrArraySize != 1 || desc_.MipLevels != 1 || desc_.Format != DXGI_FORMAT_UNKNOWN || desc_.SampleDesc.Count != 1 ||
        desc_.SampleDesc.Quality != 0 || desc_.Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR) {
        error = "a buffer description needs a nonzero width, height, depth and mips of 1, format UNKNOWN, one sample and the row-major layout";
        return false;
    }
    if (desc_.Alignment != 0 && desc_.Alignment != kBufferPlacementAlignment) {
        error = "buffer alignment " + std::to_string(desc_.Alignment) + " is not 0 or 64 KiB";
        return false;
    }
    const UINT unsupported = static_cast<UINT>(desc_.Flags) & ~allowedFlags;
    if (unsupported != 0) {
        error = "buffer flags " + std::to_string(unsupported) + " are not supported";
        return false;
    }
    return true;
}

namespace {

void CountTitleBuffer(std::uint64_t address, const D3D12_RESOURCE_DESC& desc, bool added) {
    constexpr std::uint64_t kPage = 64ull << 10;
    const int kind = (static_cast<UINT>(desc.Flags) & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0 ? 1 : 0;
    const std::uint64_t bytes = desc.Width;
    const std::uint64_t first = (address + kPage - 1) / kPage * kPage;
    const std::uint64_t last = (address + bytes) / kPage * kPage;
    const std::uint64_t pages = last > first ? last - first : 0;
    const std::uint64_t large = bytes >= kPage ? bytes : 0;
    LiveStats& stats = Stats();
    if (added) {
        stats.titleBufferBytes[kind] += bytes;
        ++stats.titleBufferCount[kind];
        stats.titleBufferLargeBytes[kind] += large;
        stats.titleBufferPageBytes[kind] += pages;
    } else {
        stats.titleBufferBytes[kind] -= bytes;
        --stats.titleBufferCount[kind];
        stats.titleBufferLargeBytes[kind] -= large;
        stats.titleBufferPageBytes[kind] -= pages;
    }
}

}

HRESULT Resource::CreateAtTitleAddress(const ResourceContext& context, IUnknown* parent, std::uint64_t address, const D3D12_RESOURCE_DESC& desc, REFIID riid, void** resource,
                                       std::string& error) {
    if (resource == nullptr) {
        error = "no output for the resource";
        return E_POINTER;
    }
    *resource = nullptr;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        AbxUnsupported("ID3D12Device (resource at a title address)", "a texture at a title address: its tiling and Vulkan image are not mapped yet");
    }
    if (address == 0) {
        error = "a null title address";
        return E_INVALIDARG;
    }
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) != sizeof(region) || region.State == MEM_FREE) {
        error = "the title address is not reserved or committed memory";
        return E_INVALIDARG;
    }
    if (region.State != MEM_COMMIT) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            AbxLog("d3d12_x", "buffers are placed at reserved title addresses too (first 0x%llx): their pages are the GPU-only ones the title maps through command queue slot 31, located when the buffer is used",
                   static_cast<unsigned long long>(address));
        }
    }
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0, 1, 1};
    Resource* created = new Resource(context, parent, heap, D3D12_HEAP_FLAG_NONE, desc);
    if (!created->CheckBufferDesc(kBufferFlags | ~kDesktopResourceFlags, error)) {
        created->Release();
        return E_INVALIDARG;
    }
    const UINT consoleFlags = static_cast<UINT>(desc.Flags) & ~kDesktopResourceFlags;
    if (consoleFlags != 0) {
        AbxTrace("resource at title address 0x%llx: console-only flags 0x%x recorded, not applied", static_cast<unsigned long long>(address), consoleFlags);
    }
    created->titleAddress_ = address;
    created->desc_.Alignment = kBufferPlacementAlignment;
    CountTitleBuffer(address, created->desc_, true);
    if ((static_cast<UINT>(desc.Flags) & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0 && context.addresses != nullptr) {
        context.addresses->WatchBuffer(address, created->desc_.Width, true);
    }
    const HRESULT result = created->QueryInterface(riid, resource);
    created->Release();
    if (FAILED(result)) error = "the resource does not implement the requested interface";
    return result;
}

std::optional<BufferRef> Resource::BufferSpan(UINT64 offset, UINT64 bytes) const {
    if (offset > desc_.Width || bytes > desc_.Width - offset) return std::nullopt;
    if (titleAddress_ == 0) return BufferRef{context_.window.Buffer(), offset_ + offset, bytes};
    if (context_.addresses == nullptr) return std::nullopt;
    std::uint64_t available = 0;
    const std::optional<TitleAddressSpace::Location> located = context_.addresses->LocateRun(titleAddress_ + offset, available, bytes);
    if (!located) return std::nullopt;
    return BufferRef{located->buffer, located->offset, (std::min)(available, static_cast<std::uint64_t>(bytes))};
}

BufferRef Resource::Buffer() const {
    if (titleAddress_ != 0) {
        if (context_.addresses != nullptr) {
            if (const std::optional<TitleAddressSpace::Location> located = context_.addresses->Locate(titleAddress_, desc_.Width)) {
                return {located->buffer, located->offset, desc_.Width};
            }
        }
        char text[160];
        std::snprintf(text, sizeof(text), "GPU access to a buffer at title address 0x%llx (%llu bytes) that is in no GPU-visible title memory",
                      static_cast<unsigned long long>(titleAddress_), static_cast<unsigned long long>(desc_.Width));
        AbxUnsupported("ID3D12Resource", text);
    }
    return {context_.window.Buffer(), offset_, desc_.Width};
}

bool Resource::CreateBuffer(std::string& error) {
    if (!CheckBufferDesc(kBufferFlags, error)) {
        return false;
    }
    const std::uint64_t bytes = (desc_.Width + kBufferPlacementAlignment - 1) / kBufferPlacementAlignment * kBufferPlacementAlignment;
    const std::optional<std::uint64_t> offset = context_.arena.Allocate(bytes, kBufferPlacementAlignment);
    if (!offset) {
        error = "the buffer window has no free range of " + std::to_string(bytes) + " bytes";
        return false;
    }
    offset_ = *offset;
    allocated_ = true;
    desc_.Alignment = kBufferPlacementAlignment;
    if ((static_cast<UINT>(heapFlags_) & D3D12_HEAP_FLAG_CREATE_NOT_ZEROED) == 0) {
        std::memset(context_.window.Base() + offset_, 0, static_cast<std::size_t>(bytes));
    }
    return true;
}

namespace {

float TargetPriority() {
    static const float priority = [] {
        const char* value = AbxSetting("d3d12_x.pageable_video_memory");
        return value != nullptr && std::strtoul(value, nullptr, 10) == 2 ? 0.75f : 0.5f;
    }();
    return priority;
}

bool TextureInitWaits() {
    static const bool waits = [] {
        const char* value = AbxSetting("d3d12_x.texture_init_wait");
        const bool on = value != nullptr && std::strtoul(value, nullptr, 10) != 0;
        AbxLog("d3d12_x", "a new texture's first transition and clear are submitted %s (d3d12_x.texture_init_wait = %d)",
               on ? "and waited for, behind everything the GPU still has to do" : "without waiting for the GPU; work that uses the texture is submitted after it", on ? 1 : 0);
        return on;
    }();
    return waits;
}

void SetMemoryPriority(const VulkanDevice& device, VkDeviceMemory memory, float priority) {
    static const PFN_vkSetDeviceMemoryPriorityEXT set = reinterpret_cast<PFN_vkSetDeviceMemoryPriorityEXT>(device.DeviceLoader()(device.Handle(), "vkSetDeviceMemoryPriorityEXT"));
    if (set != nullptr && memory != VK_NULL_HANDLE) set(device.Handle(), memory, priority);
}

}

bool Resource::CreateTexture(D3D12_RESOURCE_STATES initial, std::string& error) {
    if (heap_.Type != D3D12_HEAP_TYPE_DEFAULT) {
        error = "a texture with the driver-chosen layout must be in a default heap";
        return false;
    }
    std::optional<TextureImage> image = TranslateTextureDesc(desc_, error);
    if (!image) {
        return false;
    }
    if (desc_.Format == kConsoleD16UnormS8Uint) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            AbxLog("d3d12_x", "console format 118 (16-bit depth, 8-bit stencil) is stored as Vulkan D32_SFLOAT_S8_UINT: depth keeps at least 16-bit precision, "
                              "but depth bias follows float rules and copies of these textures are refused");
        }
    }
    if (image->depthUnorderedAccess && (context_.device.OptimalFeatures(image->info.format) & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0) {
        image->info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    for (const std::string& feature : image->features) {
        if (!context_.device.HasFeature(feature)) {
            error = "the texture needs " + feature + ", which the device does not offer";
            return false;
        }
    }
    const DeviceFunctions& vk = context_.device.Vk();
    texture_.desc = desc_;
    texture_.layout = *image;
    if (vk.createImage(context_.device.Handle(), &image->info, nullptr, &texture_.image) != VK_SUCCESS) {
        texture_.image = VK_NULL_HANDLE;
        error = "vkCreateImage failed";
        return false;
    }
    VkMemoryRequirements needs{};
    vk.getImageMemoryRequirements(context_.device.Handle(), texture_.image, &needs);
    const std::optional<std::uint32_t> type = context_.device.MemoryType(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    constexpr UINT kTargetFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    constexpr UINT kUsedWithoutListCalls = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_DEPTH_WRITE;
    constexpr VkDeviceSize kLargeTarget = 4ull << 20;
    const bool target = (static_cast<UINT>(desc_.Flags) & kTargetFlags) != 0;
    const bool byUse = context_.device.PageableMemory() && PriorityByUse();
    const std::uint64_t videoBudget = Stats().videoBudget.load(std::memory_order_relaxed);
    const bool tight = videoBudget != 0 && Stats().videoUsage.load(std::memory_order_relaxed) / 85 >= videoBudget / 100;
    const bool startsUnused = byUse && target && needs.size >= kLargeTarget && (static_cast<UINT>(initial) & kUsedWithoutListCalls) == 0 && !tight;
    VkMemoryPriorityAllocateInfoEXT priority{VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT};
    priority.priority = byUse ? (startsUnused ? 0.5f : kUsedMemoryPriority) : target ? TargetPriority() : 0.5f;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    if (context_.device.PageableMemory()) allocate.pNext = &priority;
    allocate.allocationSize = needs.size;
    allocate.memoryTypeIndex = type.value_or(0);
    const bool pooled = type && needs.size <= kPooledLimit && image->info.tiling == VK_IMAGE_TILING_OPTIMAL &&
                        PoolAllocate(context_.device, *type, needs.size, needs.alignment, poolBlock_, poolOffset_);
    if (pooled) poolSize_ = needs.size;
    VkResult allocated = pooled ? VK_SUCCESS : type ? vk.allocateMemory(context_.device.Handle(), &allocate, nullptr, &memory_) : VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (allocated != VK_SUCCESS) {
        memory_ = VK_NULL_HANDLE;
        const VkResult deviceResult = allocated;
        const std::optional<std::uint32_t> host = context_.device.SystemMemoryType(needs.memoryTypeBits);
        if (host) {
            allocate.memoryTypeIndex = *host;
            allocate.pNext = nullptr;
            allocated = vk.allocateMemory(context_.device.Handle(), &allocate, nullptr, &memory_);
        }
        if (allocated != VK_SUCCESS) {
            memory_ = VK_NULL_HANDLE;
            error = "no device memory for the texture (device-local allocation " + std::to_string(static_cast<int>(deviceResult)) + ", system-memory allocation " +
                    (host ? std::to_string(static_cast<int>(allocated)) : std::string("impossible: the image allows no system-memory type")) + ")";
            return false;
        }
        static std::atomic<std::uint64_t> spilled{0};
        const std::uint64_t count = ++spilled;
        if (count == 1 || (count & (count - 1)) == 0) {
            AbxLog("d3d12_x", "device-local memory is exhausted (VkResult %d): textures are placed in system memory the GPU reads over PCIe instead (%llu so far, this one %llu KiB)",
                   static_cast<int>(deviceResult), static_cast<unsigned long long>(count), static_cast<unsigned long long>(needs.size >> 10));
        }
    }
    textureBytes_ = needs.size;
    if (startsUnused && !pooled && memory_ != VK_NULL_HANDLE && allocate.pNext != nullptr) {
        unusedPriority_.store(true);
        Stats().unusedTargetBytes += textureBytes_;
        ++Stats().unusedTargetCount;
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            AbxLog("d3d12_x", "video memory priorities follow use (d3d12_x.memory_priority_by_use, 0 turns it off): title textures, small textures and the page tables are "
                              "allocated at a high priority; a texture of 4 MiB and more that the GPU may write starts at the normal one and is raised when a command list "
                              "first names it (a barrier, bind, clear or copy) or the title fills it, unless it is created in a render-target, depth-write or "
                              "unordered-access state or while video memory is within 15 %% of its budget; so when video memory runs over, the system moves the textures "
                              "nothing has used into system memory instead of the ones allocated last");
        }
    }
    Stats().textureBytes += textureBytes_;
    ++Stats().textureCount;
    constexpr UINT kWrittenFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    textureBucket_ = TextureBucket((static_cast<UINT>(desc_.Flags) & kWrittenFlags) != 0, textureBytes_);
    Stats().textureBucketBytes[textureBucket_] += textureBytes_;
    ++Stats().textureBucketCount[textureBucket_];
    if (needs.size >= (16ull << 20)) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 120) {
            AbxLog("d3d12_x", "DIAG large texture %llu MiB: %llux%ux%u, %u mips, format %d, %u samples, flags 0x%x", static_cast<unsigned long long>(needs.size >> 20),
                   static_cast<unsigned long long>(desc_.Width), desc_.Height, desc_.DepthOrArraySize, desc_.MipLevels, static_cast<int>(desc_.Format), desc_.SampleDesc.Count,
                   static_cast<unsigned>(desc_.Flags));
        }
    }
    const VkDeviceMemory boundMemory = pooled ? static_cast<PoolBlock*>(poolBlock_)->memory : memory_;
    if (vk.bindImageMemory(context_.device.Handle(), texture_.image, boundMemory, pooled ? poolOffset_ : 0) != VK_SUCCESS) {
        error = "vkBindImageMemory failed";
        return false;
    }
    const bool zero = (static_cast<UINT>(heapFlags_) & D3D12_HEAP_FLAG_CREATE_NOT_ZEROED) == 0 && !image->format.compressed;
    bool recorded = true;
    const bool waits = TextureInitWaits();
    const auto submit = [&](const std::function<void(VkCommandBuffer)>& record) {
        if (waits) return context_.device.Submit(record, error);
        initSerial_ = context_.device.SubmitDetached(record, error);
        return initSerial_ != 0;
    };
    const bool submitted = submit(
        [&](VkCommandBuffer commands) {
            CommandRecorder recorder(context_.device, commands);
            if (!zero) {
                recorded = recorder.InitializeTexture(texture_, initial, error);
                return;
            }
            recorded = recorder.InitializeTexture(texture_, D3D12_RESOURCE_STATE_COPY_DEST, error);
            if (!recorded) return;
            const VkImageSubresourceRange range{image->format.aspects, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            if ((image->format.aspects & VK_IMAGE_ASPECT_COLOR_BIT) != 0) {
                const VkClearColorValue black{};
                vk.cmdClearColorImage(commands, texture_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
            } else {
                const VkClearDepthStencilValue cleared{0.0f, 0};
                vk.cmdClearDepthStencilImage(commands, texture_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cleared, 1, &range);
            }
            recorded = recorder.Transition(texture_, D3D12_RESOURCE_STATE_COPY_DEST, initial, error);
        });
    if (!submitted || !recorded) {
        if (error.empty()) error = "the texture's first transition failed";
        return false;
    }
    return true;
}

namespace {

std::mutex g_gpuWrittenLock;
std::multimap<std::uint64_t, Resource*> g_gpuWritten;

}

void Resource::RegisterGpuWritten(std::uint64_t address, bool writableAtFirst) {
    const std::lock_guard guard(g_gpuWrittenLock);
    gpuWrittenAddress_ = address;
    writableAtFirst_ = writableAtFirst;
    g_gpuWritten.emplace(address, this);
}

namespace {

std::uint32_t UseClock() {
    static const ULONGLONG start = GetTickCount64();
    return static_cast<std::uint32_t>((GetTickCount64() - start) / 1000) + 1;
}

}

void Resource::NoteUse() {
    if (!unusedPriority_.load(std::memory_order_relaxed) || !unusedPriority_.exchange(false)) return;
    SetMemoryPriority(context_.device, memory_, kUsedMemoryPriority);
    Stats().unusedTargetBytes -= textureBytes_;
    --Stats().unusedTargetCount;
}

void Resource::NoteListUse(bool writes) {
    NoteUse();
    const std::uint32_t now = UseClock();
    listUse_.store(now, std::memory_order_relaxed);
    if (writes) listWrite_.store(now, std::memory_order_relaxed);
}

WrittenUse Resource::MeasureWrittenUse(std::uint32_t idleSeconds) {
    constexpr std::uint64_t kLargeBytes = 4ull << 20;
    WrittenUse use;
    const std::uint32_t now = UseClock();
    const auto add = [](WrittenUse::Part& part, std::uint64_t bytes) {
        part.bytes += bytes;
        ++part.count;
    };
    const std::lock_guard guard(g_gpuWrittenLock);
    for (const auto& [address, resource] : g_gpuWritten) {
        const std::uint64_t bytes = resource->textureBytes_;
        if (bytes < kLargeBytes) continue;
        const std::uint32_t named = resource->listUse_.load(std::memory_order_relaxed);
        const std::uint32_t written = resource->listWrite_.load(std::memory_order_relaxed);
        add(use.all, bytes);
        if (named == 0) {
            add(use.neverNamed, bytes);
            if (resource->writableAtFirst_) add(use.neverNamedWritable, bytes);
            if (resource->viewsMade_.load(std::memory_order_relaxed) == 0) add(use.neverNamedNoView, bytes);
        } else if (now - named > idleSeconds) {
            add(use.idle, bytes);
        }
        if (written == 0 && !resource->writableAtFirst_) add(use.neverWritten, bytes);
    }
    return use;
}

Resource* Resource::GpuWrittenSame(std::uint64_t address, const D3D12_RESOURCE_DESC& desc) {
    const std::lock_guard guard(g_gpuWrittenLock);
    const auto [first, last] = g_gpuWritten.equal_range(address);
    for (auto it = first; it != last; ++it) {
        const D3D12_RESOURCE_DESC& own = it->second->texture_.desc;
        if (own.Dimension == desc.Dimension && own.Width == desc.Width && own.Height == desc.Height && own.DepthOrArraySize == desc.DepthOrArraySize &&
            own.MipLevels == desc.MipLevels && own.Format == desc.Format && own.SampleDesc.Count == desc.SampleDesc.Count &&
            (static_cast<UINT>(own.Flags) & 0xFFFFu) == (static_cast<UINT>(desc.Flags) & 0xFFFFu)) {
            return it->second;
        }
    }
    return nullptr;
}

Resource* Resource::GpuWrittenAt(std::uint64_t address, const D3D12_RESOURCE_DESC& desc) {
    const std::lock_guard guard(g_gpuWrittenLock);
    Resource* found = nullptr;
    const auto [first, last] = g_gpuWritten.equal_range(address);
    for (auto it = first; it != last; ++it) {
        const D3D12_RESOURCE_DESC& own = it->second->texture_.desc;
        if (own.Dimension == desc.Dimension && own.Width == desc.Width && own.Height == desc.Height && own.DepthOrArraySize == desc.DepthOrArraySize) found = it->second;
    }
    return found;
}

Resource::~Resource() {
    context_.device.WaitDetached(initSerial_);
    if (unusedPriority_.exchange(false)) {
        Stats().unusedTargetBytes -= textureBytes_;
        --Stats().unusedTargetCount;
    }
    DescriptorWriter::ForgetResource(this);
    if (titleAddress_ != 0 && IsBuffer()) {
        CountTitleBuffer(titleAddress_, desc_, false);
        if ((static_cast<UINT>(desc_.Flags) & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0 && context_.addresses != nullptr) {
            context_.addresses->WatchBuffer(titleAddress_, desc_.Width, false);
        }
    }
    if (readBack_.load()) {
        if (TextureReadback* const readback = TextureReadback::Active()) readback->Forget(this);
    }
    if (gpuWrittenAddress_ != 0) {
        const std::lock_guard guard(g_gpuWrittenLock);
        const auto [first, last] = g_gpuWritten.equal_range(gpuWrittenAddress_);
        for (auto it = first; it != last; ++it) {
            if (it->second == this) {
                g_gpuWritten.erase(it);
                break;
            }
        }
    }
    const DeviceFunctions& vk = context_.device.Vk();
    for (const auto& [info, view] : views_) vk.destroyImageView(context_.device.Handle(), view, nullptr);
    if (depthShadowReady_.load()) {
        depthShadow_.pass->DestroySource(depthShadow_.source);
        vk.destroyImageView(context_.device.Handle(), depthShadow_.plane, nullptr);
        vk.destroyImageView(context_.device.Handle(), depthShadow_.view, nullptr);
        vk.destroyImage(context_.device.Handle(), depthShadow_.image, nullptr);
        vk.freeMemory(context_.device.Handle(), depthShadow_.memory, nullptr);
        DestroyBuffer(context_.device, depthShadow_.staging);
    }
    if (texture_.image != VK_NULL_HANDLE) vk.destroyImage(context_.device.Handle(), texture_.image, nullptr);
    if (memory_ != VK_NULL_HANDLE) vk.freeMemory(context_.device.Handle(), memory_, nullptr);
    if (poolBlock_ != nullptr) PoolFree(poolBlock_, poolOffset_, poolSize_);
    if (gpuWrittenAddress_ != 0 && textureBytes_ >= (4ull << 20)) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 400) {
            AbxLog("d3d12_x", "DIAG written release 0x%llx %llu KiB", static_cast<unsigned long long>(gpuWrittenAddress_), static_cast<unsigned long long>(textureBytes_ >> 10));
        }
    }
    if (textureBytes_ != 0) {
        Stats().textureBytes -= textureBytes_;
        --Stats().textureCount;
        Stats().textureBucketBytes[textureBucket_] -= textureBytes_;
        --Stats().textureBucketCount[textureBucket_];
    }
    if (allocated_) context_.arena.Free(offset_);
    if (titleMemory_ != nullptr && context_.titleMemory != nullptr) context_.titleMemory->release(titleMemory_, titleMemoryAttributes_);
}

void Resource::AttachTitleMemory(void* memory, std::uint64_t attributes) {
    titleMemory_ = memory;
    titleMemoryAttributes_ = attributes;
}

namespace {

bool SameView(const VkImageViewCreateInfo& a, const VkImageViewCreateInfo& b) {
    const VkComponentMapping& x = a.components;
    const VkComponentMapping& y = b.components;
    const VkImageSubresourceRange& r = a.subresourceRange;
    const VkImageSubresourceRange& s = b.subresourceRange;
    return a.flags == b.flags && a.viewType == b.viewType && a.format == b.format && x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a && r.aspectMask == s.aspectMask &&
           r.baseMipLevel == s.baseMipLevel && r.levelCount == s.levelCount && r.baseArrayLayer == s.baseArrayLayer && r.layerCount == s.layerCount;
}

}

VkImageView Resource::View(const VkImageViewCreateInfo& info, std::string& error) {
    if (IsBuffer() || info.pNext != nullptr) {
        error = IsBuffer() ? "a buffer has no image views" : "image view extensions are not supported";
        return VK_NULL_HANDLE;
    }
    std::lock_guard<std::mutex> guard(viewLock_);
    for (const auto& [known, view] : views_) {
        if (SameView(known, info)) return view;
    }
    VkImageViewCreateInfo create = info;
    create.image = texture_.image;
    VkImageView view = VK_NULL_HANDLE;
    if (context_.device.Vk().createImageView(context_.device.Handle(), &create, nullptr, &view) != VK_SUCCESS) {
        error = "vkCreateImageView failed";
        return VK_NULL_HANDLE;
    }
    views_.emplace_back(create, view);
    viewsMade_.fetch_add(1, std::memory_order_relaxed);
    return view;
}

unsigned long Resource::DepthUnorderedAccess() {
    static const unsigned long setting = [] {
        const char* const value = AbxSetting("d3d12_x.depth_unordered_access");
        return value == nullptr ? 0ul : std::strtoul(value, nullptr, 10);
    }();
    return setting;
}

Resource::DepthShadow* Resource::EnsureDepthShadow(std::string& error) {
    std::lock_guard<std::mutex> guard(viewLock_);
    if (depthShadowReady_.load()) return &depthShadow_;
    const VkImageCreateInfo& depth = texture_.layout.info;
    if (IsBuffer() || texture_.image == VK_NULL_HANDLE || (depth.format != VK_FORMAT_D32_SFLOAT_S8_UINT && depth.format != VK_FORMAT_D32_SFLOAT)) {
        error = "the depth plane is not stored as 32-bit float, so no color image holds the same values";
        return nullptr;
    }
    if (depth.imageType != VK_IMAGE_TYPE_2D || depth.mipLevels != 1 || depth.arrayLayers != 1 || depth.samples != VK_SAMPLE_COUNT_1_BIT) {
        error = "the depth texture has several mips, layers or samples";
        return nullptr;
    }
    const DeviceFunctions& vk = context_.device.Vk();
    DepthShadow& shadow = depthShadow_;
    DepthPlanePass* const pass = DepthPlanePass::For(context_.device);
    if (pass == nullptr) {
        error = "the device cannot draw written depth values into a depth plane";
        return nullptr;
    }
    shadow.stencil = (texture_.layout.format.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    shadow.pipeline = pass->Pipeline(depth.format, shadow.stencil, error);
    if (shadow.pipeline == VK_NULL_HANDLE) return nullptr;
    const auto release = [&]() {
        pass->DestroySource(shadow.source);
        if (shadow.plane != VK_NULL_HANDLE) vk.destroyImageView(context_.device.Handle(), shadow.plane, nullptr);
        if (shadow.view != VK_NULL_HANDLE) vk.destroyImageView(context_.device.Handle(), shadow.view, nullptr);
        if (shadow.image != VK_NULL_HANDLE) vk.destroyImage(context_.device.Handle(), shadow.image, nullptr);
        if (shadow.memory != VK_NULL_HANDLE) vk.freeMemory(context_.device.Handle(), shadow.memory, nullptr);
        shadow.plane = VK_NULL_HANDLE;
        shadow.view = VK_NULL_HANDLE;
        shadow.image = VK_NULL_HANDLE;
        shadow.memory = VK_NULL_HANDLE;
    };
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R32_SFLOAT;
    info.extent = {depth.extent.width, depth.extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk.createImage(context_.device.Handle(), &info, nullptr, &shadow.image) != VK_SUCCESS) {
        shadow.image = VK_NULL_HANDLE;
        error = "vkCreateImage failed for the color image behind the depth plane";
        return nullptr;
    }
    VkMemoryRequirements needs{};
    vk.getImageMemoryRequirements(context_.device.Handle(), shadow.image, &needs);
    const std::optional<std::uint32_t> type = context_.device.MemoryType(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryPriorityAllocateInfoEXT priority{VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT};
    priority.priority = kUsedMemoryPriority;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    if (context_.device.PageableMemory() && PriorityByUse()) allocate.pNext = &priority;
    allocate.allocationSize = needs.size;
    allocate.memoryTypeIndex = type.value_or(0);
    if (!type || vk.allocateMemory(context_.device.Handle(), &allocate, nullptr, &shadow.memory) != VK_SUCCESS) {
        shadow.memory = VK_NULL_HANDLE;
        release();
        error = "no device memory for the color image behind the depth plane";
        return nullptr;
    }
    if (vk.bindImageMemory(context_.device.Handle(), shadow.image, shadow.memory, 0) != VK_SUCCESS) {
        release();
        error = "vkBindImageMemory failed for the color image behind the depth plane";
        return nullptr;
    }
    shadow.info = VkImageViewCreateInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    shadow.info.image = shadow.image;
    shadow.info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    shadow.info.format = VK_FORMAT_R32_SFLOAT;
    shadow.info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    shadow.info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vk.createImageView(context_.device.Handle(), &shadow.info, nullptr, &shadow.view) != VK_SUCCESS) {
        shadow.view = VK_NULL_HANDLE;
        release();
        error = "vkCreateImageView failed for the color image behind the depth plane";
        return nullptr;
    }
    VkImageViewCreateInfo plane{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    plane.image = texture_.image;
    plane.viewType = VK_IMAGE_VIEW_TYPE_2D;
    plane.format = depth.format;
    plane.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    plane.subresourceRange = {texture_.layout.format.aspects, 0, 1, 0, 1};
    if (vk.createImageView(context_.device.Handle(), &plane, nullptr, &shadow.plane) != VK_SUCCESS) {
        shadow.plane = VK_NULL_HANDLE;
        release();
        error = "vkCreateImageView failed for the depth plane as a depth target";
        return nullptr;
    }
    if (!pass->CreateSource(shadow.view, shadow.source, error)) {
        release();
        return nullptr;
    }
    const VkDeviceSize bytes = VkDeviceSize{4} * depth.extent.width * depth.extent.height;
    if (!CreateHostBuffer(context_.device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, shadow.staging, error)) {
        release();
        return nullptr;
    }
    const VkImage created = shadow.image;
    const bool cleared = context_.device.Submit(
        [&vk, created](VkCommandBuffer commands) {
            VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = created;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &barrier;
            vk.cmdPipelineBarrier2(commands, &dependency);
            const VkClearColorValue zero{};
            const VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vk.cmdClearColorImage(commands, created, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &whole);
        },
        error);
    if (!cleared) {
        DestroyBuffer(context_.device, shadow.staging);
        release();
        error = "the color image behind the depth plane could not be cleared: " + error;
        return nullptr;
    }
    shadow.pass = pass;
    shadow.extent = info.extent;
    depthShadowReady_.store(true, std::memory_order_release);
    return &shadow;
}

std::size_t Resource::Views() const {
    std::lock_guard<std::mutex> guard(viewLock_);
    return views_.size();
}

bool Resource::Implements(REFIID riid) const {
    return DeviceChildBase::Implements(riid) || riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12Resource);
}

HRESULT STDMETHODCALLTYPE Resource::Map(UINT subresource, const D3D12_RANGE*, void** data) {
    if (!IsBuffer() || subresource != 0) {
        return E_INVALIDARG;
    }
    ++maps_;
    if (data != nullptr) {
        *data = titleAddress_ != 0 ? reinterpret_cast<void*>(titleAddress_) : static_cast<void*>(context_.window.Base() + offset_);
    }
    return S_OK;
}

void STDMETHODCALLTYPE Resource::Unmap(UINT subresource, const D3D12_RANGE*) {
    if (IsBuffer() && subresource == 0 && maps_ > 0) {
        --maps_;
    }
}

D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE Resource::GetGPUVirtualAddress() {
    if (titleAddress_ != 0) return titleAddress_;
    if (!IsBuffer()) return textureAddress_;
    return context_.window.Start() + offset_;
}

HRESULT STDMETHODCALLTYPE Resource::WriteToSubresource(UINT, const D3D12_BOX*, const void*, UINT, UINT) {
    AbxUnsupported("ID3D12Resource::WriteToSubresource", "CPU writes to texture subresources");
}

HRESULT STDMETHODCALLTYPE Resource::ReadFromSubresource(void*, UINT, UINT, UINT, const D3D12_BOX*) {
    AbxUnsupported("ID3D12Resource::ReadFromSubresource", "CPU reads from texture subresources");
}

HRESULT STDMETHODCALLTYPE Resource::GetHeapProperties(D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS* flags) {
    if (properties != nullptr) *properties = heap_;
    if (flags != nullptr) *flags = heapFlags_;
    return S_OK;
}

}
