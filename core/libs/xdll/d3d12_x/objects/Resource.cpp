#include "Resource.hpp"

#include "AbxApi.hpp"
#include "ResourceState.hpp"
#include "TextureDesc.hpp"

#include <cstring>

namespace abx::d3d12x {

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
    if (!built) {
        created->Release();
        return E_INVALIDARG;
    }
    const HRESULT result = created->QueryInterface(riid, resource);
    created->Release();
    if (FAILED(result)) error = "the resource does not implement the requested interface";
    return result;
}

bool Resource::CreateBuffer(std::string& error) {
    if (desc_.Width == 0 || desc_.Height != 1 || desc_.DepthOrArraySize != 1 || desc_.MipLevels != 1 || desc_.Format != DXGI_FORMAT_UNKNOWN || desc_.SampleDesc.Count != 1 ||
        desc_.SampleDesc.Quality != 0 || desc_.Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR) {
        error = "a buffer description needs a nonzero width, height, depth and mips of 1, format UNKNOWN, one sample and the row-major layout";
        return false;
    }
    if (desc_.Alignment != 0 && desc_.Alignment != kBufferPlacementAlignment) {
        error = "buffer alignment " + std::to_string(desc_.Alignment) + " is not 0 or 64 KiB";
        return false;
    }
    const UINT unsupported = static_cast<UINT>(desc_.Flags) & ~static_cast<UINT>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE |
                                                                                D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
    if (unsupported != 0) {
        error = "buffer flags " + std::to_string(unsupported) + " are not supported";
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
    const std::optional<TextureImage> image = TranslateTextureDesc(desc_, error);
    if (!image) {
        return false;
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
    if (!type || vk.allocateMemory(context_.device.Handle(), &allocate, nullptr, &memory_) != VK_SUCCESS) {
        memory_ = VK_NULL_HANDLE;
        error = "no device memory for the texture";
        return false;
    }
    if (vk.bindImageMemory(context_.device.Handle(), texture_.image, memory_, 0) != VK_SUCCESS) {
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

Resource::~Resource() {
    const DeviceFunctions& vk = context_.device.Vk();
    if (texture_.image != VK_NULL_HANDLE) vk.destroyImage(context_.device.Handle(), texture_.image, nullptr);
    if (memory_ != VK_NULL_HANDLE) vk.freeMemory(context_.device.Handle(), memory_, nullptr);
    if (allocated_) context_.arena.Free(offset_);
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
        *data = context_.window.Base() + offset_;
    }
    return S_OK;
}

void STDMETHODCALLTYPE Resource::Unmap(UINT subresource, const D3D12_RANGE*) {
    if (IsBuffer() && subresource == 0 && maps_ > 0) {
        --maps_;
    }
}

D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE Resource::GetGPUVirtualAddress() {
    return IsBuffer() ? context_.window.Start() + offset_ : 0;
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
