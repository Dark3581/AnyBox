#include "Resource.hpp"

#include "AbxApi.hpp"
#include "DescriptorWriter.hpp"
#include "LiveStats.hpp"
#include "ResourceState.hpp"
#include "GraphicsState.hpp"
#include "TextureDesc.hpp"

#include <algorithm>
#include <iterator>
#include <vector>
#include <memory>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <optional>
#include <map>
#include <mutex>

namespace abx::d3d12x {

namespace {

constexpr VkDeviceSize kPoolBlockBytes = 64ull << 20;
constexpr VkDeviceSize kPooledLimit = 1ull << 20;

struct PoolBlock {
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

bool PoolAllocate(const VulkanDevice& device, std::uint32_t type, VkDeviceSize size, VkDeviceSize alignment, void*& handle, VkDeviceSize& offset) {
    TexturePool& pool = Pool();
    const std::lock_guard guard(pool.lock);
    for (const std::unique_ptr<PoolBlock>& block : pool.blocks) {
        if (block->type == type && Carve(*block, size, (std::max)(alignment, VkDeviceSize{1}), offset)) {
            handle = block.get();
            return true;
        }
    }
    auto block = std::make_unique<PoolBlock>();
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = kPoolBlockBytes;
    allocate.memoryTypeIndex = type;
    if (device.Vk().allocateMemory(device.Handle(), &allocate, nullptr, &block->memory) != VK_SUCCESS) return false;
    block->type = type;
    block->free[0] = kPoolBlockBytes;
    Stats().poolBlockBytes += kPoolBlockBytes;
    if (!Carve(*block, size, (std::max)(alignment, VkDeviceSize{1}), offset)) return false;
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
    const std::optional<TitleAddressSpace::Location> located = context_.addresses->LocateRun(titleAddress_ + offset, available);
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
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
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
    const bool submitted = context_.device.Submit(
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
        },
        error);
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

void Resource::RegisterGpuWritten(std::uint64_t address) {
    const std::lock_guard guard(g_gpuWrittenLock);
    gpuWrittenAddress_ = address;
    g_gpuWritten.emplace(address, this);
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
    DescriptorWriter::ForgetResource(this);
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
    return view;
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
