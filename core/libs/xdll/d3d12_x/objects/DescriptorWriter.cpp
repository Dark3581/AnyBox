#include "DescriptorWriter.hpp"

#include "AbxApi.hpp"
#include "BindlessSet.hpp"
#include "BufferDescriptor.hpp"
#include "ImageDescriptor.hpp"
#include "ViewDesc.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <optional>
#include <string>

namespace abx::d3d12x {

namespace {

const char* const kImageWordsUnknown =
    "image and sampler descriptor words in a shader-visible heap need the console's image (T#) and sampler (S#) descriptor layouts, which are not sourced yet";

Resource* Ours(ID3D12Resource* resource, const char* function) {
    if (resource == nullptr) {
        return nullptr;
    }
    Resource* ours = dynamic_cast<Resource*>(resource);
    if (ours == nullptr) {
        AbxUnsupported(function, "a resource that AnyBox did not create");
    }
    return ours;
}

void Check(bool ok, const char* function, const std::string& error) {
    if (!ok) {
        AbxUnsupported(function, error.c_str());
    }
}

std::optional<std::uint32_t> ImageBinding(VkImageViewType type, ImageComponent component, bool storage) {
    const std::uint32_t offset = component == ImageComponent::Uint ? 1u : component == ImageComponent::Sint ? 2u : 0u;
    if (!storage && component == ImageComponent::Float) {
        switch (type) {
        case VK_IMAGE_VIEW_TYPE_1D: return 5u;
        case VK_IMAGE_VIEW_TYPE_2D: return 0u;
        case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return 3u;
        case VK_IMAGE_VIEW_TYPE_3D: return 6u;
        default: return std::nullopt;
        }
    }
    switch (type) {
    case VK_IMAGE_VIEW_TYPE_2D: return 8u + offset;
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return 11u + offset;
    case VK_IMAGE_VIEW_TYPE_3D: return 14u + offset;
    case VK_IMAGE_VIEW_TYPE_1D: return 17u + offset;
    default: return std::nullopt;
    }
}

std::optional<std::uint32_t> ImageType(VkImageViewType type) {
    switch (type) {
    case VK_IMAGE_VIEW_TYPE_1D: return kImageType1D;
    case VK_IMAGE_VIEW_TYPE_2D: return kImageType2D;
    case VK_IMAGE_VIEW_TYPE_3D: return kImageType3D;
    case VK_IMAGE_VIEW_TYPE_CUBE: return kImageTypeCube;
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return kImageType1DArray;
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return kImageType2DArray;
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return kImageTypeCube;
    default: return std::nullopt;
    }
}

VkExtent2D MipExtent(const D3D12_RESOURCE_DESC& desc, std::uint32_t mip) {
    const std::uint64_t width = (std::max)(desc.Width >> mip, std::uint64_t{1});
    const std::uint32_t height = (std::max)(desc.Height >> mip, 1u);
    return {static_cast<std::uint32_t>(width), height};
}

}

DescriptorWriter::~DescriptorWriter() {
    for (const auto& [desc, sampler] : samplers_) {
        context_.device.Vk().destroySampler(context_.device.Handle(), sampler, nullptr);
    }
}

std::size_t DescriptorWriter::Samplers() const {
    std::lock_guard<std::mutex> guard(samplerLock_);
    return samplers_.size();
}

DescriptorWriter::Target DescriptorWriter::Resolve(D3D12_CPU_DESCRIPTOR_HANDLE handle, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, const char* function) {
    Target target;
    target.heap = DescriptorHeap::Find(handle.ptr, target.index);
    if (target.heap == nullptr) {
        AbxUnsupported(function, "a descriptor handle outside every descriptor heap AnyBox created");
    }
    if (target.heap->Type() != type) {
        AbxUnsupported(function, "a descriptor handle in a heap of another type");
    }
    if (count > target.heap->Count() - target.index) {
        AbxUnsupported(function, "a descriptor range that runs past the end of its heap");
    }
    return target;
}

void DescriptorWriter::WriteWords(const Target& target, const void* words, std::size_t bytes) {
    std::uint8_t* slot = target.heap->Slot(target.index);
    if (bytes != 0) std::memcpy(slot, words, bytes);
    std::memset(slot + bytes, 0, target.heap->Increment() - bytes);
}

void DescriptorWriter::RequireImageWords(const Target& target, HeapEntry kind, const char* function) {
    if (target.heap->ShaderVisible() && (kind == HeapEntry::Image || kind == HeapEntry::Sampler)) {
        AbxUnsupported(function, kImageWordsUnknown);
    }
}

void DescriptorWriter::CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    const char* function = "ID3D12Device::CreateConstantBufferView";
    const Target target = Resolve(destination, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, function);
    if (desc == nullptr) {
        AbxUnsupported(function, "a constant buffer view without a description");
    }
    DescriptorRecord record;
    if (desc->BufferLocation == 0) {
        WriteWords(target, nullptr, 0);
        target.heap->Record(target.index) = record;
        return;
    }
    if (!context_.window.Contains(desc->BufferLocation, desc->SizeInBytes)) {
        if (context_.titleMemory == nullptr) {
            AbxUnsupported(function, "a constant buffer outside the title buffer window");
        }
        AbxTrace("%s: constant buffer at title address 0x%llx (%u bytes) has no Vulkan backing yet", function, static_cast<unsigned long long>(desc->BufferLocation),
                 desc->SizeInBytes);
    }
    std::string error;
    const std::optional<BufferDescriptorWords> words = ConstantBufferDescriptor(*desc, error);
    Check(words.has_value(), function, error);
    WriteWords(target, words->data(), kBufferDescriptorBytes);
    record.kind = HeapEntry::Buffer;
    target.heap->Record(target.index) = record;
}

void DescriptorWriter::WriteImageView(Resource* resource, const std::optional<ImageView>& view, bool unorderedAccess, const Target& target, const char* function,
                                      std::string& error) {
    Check(view.has_value(), function, error);
    for (const std::string& feature : view->features) {
        if (!context_.device.HasFeature(feature)) {
            AbxUnsupported(function, ("the view needs the device feature " + feature + ", which this GPU or driver lacks").c_str());
        }
    }
    const bool encode = context_.bindless != nullptr;
    if (!encode) RequireImageWords(target, HeapEntry::Image, function);
    DescriptorRecord record;
    record.kind = HeapEntry::Image;
    record.unorderedAccess = unorderedAccess;
    record.resource = resource;
    record.view = resource->View(view->info, error);
    Check(record.view != VK_NULL_HANDLE, function, error);
    if (encode) {
        const ImageDescriptorWords words = BindImage(resource, view->info, record.view, unorderedAccess, function);
        WriteWords(target, words.data(), sizeof(words));
    } else {
        WriteWords(target, nullptr, 0);
    }
    target.heap->Record(target.index) = record;
}

ImageDescriptorWords DescriptorWriter::BindImage(Resource* resource, const VkImageViewCreateInfo& info, VkImageView view, bool unorderedAccess, const char* function) {
    const std::lock_guard guard(imageLock_);
    const auto known = images_.find(view);
    if (known != images_.end()) return known->second.words;
    const VkImageCreateInfo& image = resource->Texture().layout.info;
    const bool multisampled = image.samples != VK_SAMPLE_COUNT_1_BIT;
    std::optional<ImageFormatInfo> format = ImageFormatOf(info.format);
    if (!format && (info.format == VK_FORMAT_D32_SFLOAT_S8_UINT || info.format == VK_FORMAT_D32_SFLOAT)) {
        if (info.subresourceRange.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT) format = ImageFormatOf(VK_FORMAT_R32_SFLOAT);
        if (info.subresourceRange.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT) format = ImageFormatOf(VK_FORMAT_R8_UINT);
    }
    if (!format) {
        AbxUnsupported(function, ("Vulkan format " + std::to_string(static_cast<int>(info.format)) + " has no image descriptor format code AnyBox has sourced").c_str());
    }
    const bool cube = info.viewType == VK_IMAGE_VIEW_TYPE_CUBE || info.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    const VkImageViewType bindingType = cube ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : info.viewType;
    const std::optional<std::uint32_t> binding = multisampled ? std::optional<std::uint32_t>(0u) : ImageBinding(bindingType, format->component, unorderedAccess);
    std::optional<std::uint32_t> type = ImageType(info.viewType);
    if (multisampled && type) type = *type == kImageType2DArray ? kImageType2DMsaaArray : kImageType2DMsaa;
    if (!binding || !type) {
        AbxUnsupported(function, ("an image view of type " + std::to_string(static_cast<int>(info.viewType)) + " with that component type has no bindless binding").c_str());
    }
    const VkImageSubresourceRange& range = info.subresourceRange;
    const std::uint32_t levels = range.levelCount == VK_REMAINING_MIP_LEVELS ? image.mipLevels - range.baseMipLevel : range.levelCount;
    const std::uint32_t layers = range.layerCount == VK_REMAINING_ARRAY_LAYERS ? image.arrayLayers - range.baseArrayLayer : range.layerCount;
    ImageDescriptorFields fields;
    fields.base = nextImageBase_++;
    fields.format = format->code;
    fields.width = image.extent.width;
    fields.height = image.extent.height;
    fields.depth = info.viewType == VK_IMAGE_VIEW_TYPE_3D ? image.extent.depth
                   : (info.viewType == VK_IMAGE_VIEW_TYPE_2D || info.viewType == VK_IMAGE_VIEW_TYPE_1D) ? 1u
                                                                                                         : range.baseArrayLayer + layers;
    const VkComponentSwizzle swizzles[] = {info.components.r, info.components.g, info.components.b, info.components.a};
    for (std::uint32_t component = 0; component < 4; ++component) {
        const std::optional<std::uint32_t> select = ComponentSelect(swizzles[component], 4 + component);
        if (!select) AbxUnsupported(function, "a component swizzle the image descriptor cannot express");
        fields.select[component] = *select;
    }
    fields.baseLevel = range.baseMipLevel;
    fields.lastLevel = range.baseMipLevel + levels - 1;
    fields.type = *type;
    fields.baseArray = range.baseArrayLayer;
    fields.maxMip = image.mipLevels - 1;
    if (multisampled) {
        std::uint32_t log2 = 0;
        while ((1u << (log2 + 1)) <= static_cast<std::uint32_t>(image.samples)) ++log2;
        fields.baseLevel = 0;
        fields.lastLevel = log2;
        fields.maxMip = static_cast<std::uint32_t>(image.samples);
    }
    const std::optional<ImageDescriptorWords> words = EncodeImageDescriptor(fields);
    if (!words) AbxUnsupported(function, "an image whose size, mips or layers do not fit the image descriptor");
    if (multisampled) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            AbxLog("d3d12_x", "multisampled textures get image descriptor words but no bindless slot: the title layout has no multisampled image binding");
        }
        images_.emplace(view, BoundImage{*words});
        return *words;
    }
    std::uint32_t& slot = nextImageSlot_.try_emplace(*binding, 1u).first->second;
    std::string error;
    VkImageView bound = view;
    if (cube) {
        VkImageViewCreateInfo layers = info;
        layers.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        bound = resource->View(layers, error);
        Check(bound != VK_NULL_HANDLE, function, error);
    }
    Check(context_.bindless->WriteImage(*binding, slot, bound, error), function, error);
    std::uint32_t source[8];
    std::copy(words->begin(), words->end(), source);
    if (!context_.bindless->Table().Insert(ImageKey(source), slot)) {
        AbxUnsupported(function, "the bindless descriptor table refused a new image key");
    }
    Check(context_.bindless->Upload(error), function, error);
    ++slot;
    images_.emplace(view, BoundImage{*words});
    return *words;
}

void DescriptorWriter::CreateShaderResourceView(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    const char* function = "ID3D12Device::CreateShaderResourceView";
    const Target target = Resolve(destination, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, function);
    Resource* ours = Ours(resource, function);
    std::string error;
    if (ours == nullptr) {
        if (desc == nullptr) {
            AbxUnsupported(function, "a null shader resource view without a description");
        }
        if (desc->ViewDimension == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE) {
            AbxUnsupported(function, "ray tracing acceleration structures");
        }
        WriteWords(target, nullptr, 0);
        target.heap->Record(target.index) = DescriptorRecord{};
        return;
    }
    if (ours->IsBuffer()) {
        if (desc == nullptr) {
            AbxUnsupported(function, "a buffer shader resource view without a description");
        }
        const std::optional<BufferDescriptorWords> words = ShaderResourceBufferDescriptor(ours->GetGPUVirtualAddress(), ours->Width(), *desc, error);
        Check(words.has_value(), function, error);
        WriteWords(target, words->data(), kBufferDescriptorBytes);
        DescriptorRecord record;
        record.kind = HeapEntry::Buffer;
        record.resource = ours;
        target.heap->Record(target.index) = record;
        return;
    }
    WriteImageView(ours, ShaderResourceView(ours->Texture().desc, desc, error), false, target, function, error);
}

void DescriptorWriter::CreateShaderResourceViewAt(std::uint64_t address, std::uint64_t bytes, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    const char* function = "ID3D12Device::CreateShaderResourceView (buffer at an address)";
    const Target target = Resolve(destination, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, function);
    std::string error;
    const std::optional<BufferDescriptorWords> words = ShaderResourceBufferDescriptor(address, bytes, desc, error);
    Check(words.has_value(), function, error);
    WriteWords(target, words->data(), kBufferDescriptorBytes);
    DescriptorRecord record;
    record.kind = HeapEntry::Buffer;
    target.heap->Record(target.index) = record;
}

void DescriptorWriter::CreateUnorderedAccessView(ID3D12Resource* resource, ID3D12Resource* counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
                                                 D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    const char* function = "ID3D12Device::CreateUnorderedAccessView";
    const Target target = Resolve(destination, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, function);
    Resource* ours = Ours(resource, function);
    Ours(counter, function);
    std::string error;
    if (ours == nullptr) {
        if (desc == nullptr) {
            AbxUnsupported(function, "a null unordered access view without a description");
        }
        WriteWords(target, nullptr, 0);
        target.heap->Record(target.index) = DescriptorRecord{};
        return;
    }
    if (ours->IsBuffer()) {
        if (desc == nullptr) {
            AbxUnsupported(function, "a buffer unordered access view without a description");
        }
        const std::optional<BufferDescriptorWords> words = UnorderedAccessBufferDescriptor(ours->GetGPUVirtualAddress(), ours->Width(), *desc, counter != nullptr, error);
        Check(words.has_value(), function, error);
        WriteWords(target, words->data(), kBufferDescriptorBytes);
        DescriptorRecord record;
        record.kind = HeapEntry::Buffer;
        record.unorderedAccess = true;
        record.resource = ours;
        target.heap->Record(target.index) = record;
        return;
    }
    if (counter != nullptr) {
        AbxUnsupported(function, "a counter on a texture unordered access view");
    }
    WriteImageView(ours, UnorderedAccessView(ours->Texture().desc, desc, error), true, target, function, error);
}

void DescriptorWriter::CreateRenderTargetView(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    const char* function = "ID3D12Device::CreateRenderTargetView";
    const Target target = Resolve(destination, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, function);
    Resource* ours = Ours(resource, function);
    if (ours == nullptr) {
        target.heap->Record(target.index) = DescriptorRecord{};
        return;
    }
    if (ours->IsBuffer()) {
        AbxUnsupported(function, "a render target view of a buffer");
    }
    std::string error;
    const std::optional<ImageView> view = RenderTargetView(ours->Texture().desc, desc, error);
    Check(view.has_value(), function, error);
    DescriptorRecord record;
    record.kind = HeapEntry::RenderTarget;
    record.resource = ours;
    record.view = ours->View(view->info, error);
    Check(record.view != VK_NULL_HANDLE, function, error);
    record.attachment.view = record.view;
    record.attachment.extent = MipExtent(ours->Texture().desc, view->info.subresourceRange.baseMipLevel);
    record.attachment.layers = view->info.subresourceRange.layerCount;
    record.attachment.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    target.heap->Record(target.index) = record;
}

void DescriptorWriter::CreateDepthStencilView(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    const char* function = "ID3D12Device::CreateDepthStencilView";
    const Target target = Resolve(destination, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, function);
    Resource* ours = Ours(resource, function);
    if (ours == nullptr) {
        target.heap->Record(target.index) = DescriptorRecord{};
        return;
    }
    if (ours->IsBuffer()) {
        AbxUnsupported(function, "a depth-stencil view of a buffer");
    }
    std::string error;
    const std::optional<ImageView> view = DepthStencilView(ours->Texture().desc, desc, error);
    Check(view.has_value(), function, error);
    DescriptorRecord record;
    record.kind = HeapEntry::DepthStencil;
    record.resource = ours;
    record.view = ours->View(view->info, error);
    Check(record.view != VK_NULL_HANDLE, function, error);
    record.attachment.view = record.view;
    record.attachment.extent = MipExtent(ours->Texture().desc, view->info.subresourceRange.baseMipLevel);
    record.attachment.layers = view->info.subresourceRange.layerCount;
    record.attachment.aspects = ours->Texture().layout.format.aspects;
    record.attachment.readOnly = desc != nullptr ? desc->Flags : D3D12_DSV_FLAG_NONE;
    target.heap->Record(target.index) = record;
}

VkSampler DescriptorWriter::SamplerFor(const D3D12_SAMPLER_DESC& desc, std::string& error) {
    std::lock_guard<std::mutex> guard(samplerLock_);
    for (const auto& [known, sampler] : samplers_) {
        if (std::memcmp(&known, &desc, sizeof(desc)) == 0) return sampler;
    }
    std::optional<Sampler> translated = TranslateSampler(desc, error);
    if (!translated) {
        return VK_NULL_HANDLE;
    }
    for (const std::string& feature : translated->features) {
        if (!context_.device.HasFeature(feature)) {
            error = "the sampler needs the device feature " + feature + ", which this GPU or driver lacks";
            return VK_NULL_HANDLE;
        }
    }
    if (translated->usesReduction) translated->info.pNext = &translated->reduction;
    VkSampler sampler = VK_NULL_HANDLE;
    if (context_.device.Vk().createSampler(context_.device.Handle(), &translated->info, nullptr, &sampler) != VK_SUCCESS) {
        error = "vkCreateSampler failed";
        return VK_NULL_HANDLE;
    }
    samplers_.emplace_back(desc, sampler);
    return sampler;
}

void DescriptorWriter::CreateSampler(const D3D12_SAMPLER_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    const char* function = "ID3D12Device::CreateSampler";
    const Target target = Resolve(destination, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1, function);
    if (desc == nullptr) {
        AbxUnsupported(function, "a sampler without a description");
    }
    const bool encode = context_.bindless != nullptr;
    if (!encode) RequireImageWords(target, HeapEntry::Sampler, function);
    std::string error;
    DescriptorRecord record;
    record.kind = HeapEntry::Sampler;
    record.sampler = SamplerFor(*desc, error);
    Check(record.sampler != VK_NULL_HANDLE, function, error);
    if (!encode) {
        WriteWords(target, nullptr, 0);
        target.heap->Record(target.index) = record;
        return;
    }
    SamplerDescriptorWords words{};
    {
        const std::lock_guard guard(imageLock_);
        const auto known = boundSamplers_.find(record.sampler);
        if (known != boundSamplers_.end()) {
            words = known->second;
        } else {
            const std::uint32_t slot = nextSamplerSlot_;
            const std::optional<SamplerDescriptorWords> encoded = EncodeSamplerDescriptor(*desc, slot);
            if (!encoded) AbxUnsupported(function, "a sampler whose address modes or count do not fit the sampler descriptor");
            Check(context_.bindless->WriteSampler(slot, record.sampler, error), function, error);
            std::uint32_t source[4];
            std::copy(encoded->begin(), encoded->end(), source);
            if (!context_.bindless->Table().Insert(SamplerKey(source), slot)) {
                AbxUnsupported(function, "the bindless descriptor table refused a new sampler key");
            }
            Check(context_.bindless->Upload(error), function, error);
            ++nextSamplerSlot_;
            words = *encoded;
            boundSamplers_.emplace(record.sampler, words);
        }
    }
    WriteWords(target, words.data(), sizeof(words));
    target.heap->Record(target.index) = record;
}

void DescriptorWriter::CopyDescriptorsSimple(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE destination, D3D12_CPU_DESCRIPTOR_HANDLE source, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    const UINT one = count;
    CopyDescriptors(1, &destination, &one, 1, &source, &one, type);
}

void DescriptorWriter::CopyDescriptors(UINT destinationRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* destinationStarts, const UINT* destinationSizes, UINT sourceRanges,
                                       const D3D12_CPU_DESCRIPTOR_HANDLE* sourceStarts, const UINT* sourceSizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    const char* function = "ID3D12Device::CopyDescriptors";
    if ((destinationRanges != 0 && destinationStarts == nullptr) || (sourceRanges != 0 && sourceStarts == nullptr)) {
        AbxUnsupported(function, "descriptor ranges without their start handles");
    }
    const auto sizeOf = [](const UINT* sizes, UINT range) { return sizes != nullptr ? sizes[range] : 1u; };
    std::uint64_t destinationTotal = 0;
    std::uint64_t sourceTotal = 0;
    for (UINT range = 0; range < destinationRanges; ++range) destinationTotal += sizeOf(destinationSizes, range);
    for (UINT range = 0; range < sourceRanges; ++range) sourceTotal += sizeOf(sourceSizes, range);
    if (destinationTotal != sourceTotal) {
        AbxUnsupported(function, "destination and source ranges that hold different numbers of descriptors");
    }
    UINT destinationRange = 0;
    UINT sourceRange = 0;
    UINT destinationDone = 0;
    UINT sourceDone = 0;
    Target to;
    Target from;
    while (destinationRange < destinationRanges && sourceRange < sourceRanges) {
        const UINT destinationSize = sizeOf(destinationSizes, destinationRange);
        const UINT sourceSize = sizeOf(sourceSizes, sourceRange);
        if (destinationDone == destinationSize) {
            ++destinationRange;
            destinationDone = 0;
            continue;
        }
        if (sourceDone == sourceSize) {
            ++sourceRange;
            sourceDone = 0;
            continue;
        }
        if (destinationDone == 0) to = Resolve(destinationStarts[destinationRange], type, destinationSize, function);
        if (sourceDone == 0) from = Resolve(sourceStarts[sourceRange], type, sourceSize, function);
        const UINT run = (std::min)(destinationSize - destinationDone, sourceSize - sourceDone);
        for (UINT step = 0; step < run; ++step) {
            const UINT target = to.index + destinationDone + step;
            const UINT origin = from.index + sourceDone + step;
            const DescriptorRecord record = from.heap->Record(origin);
            if (context_.bindless == nullptr) RequireImageWords({to.heap, target}, record.kind, function);
            std::memmove(to.heap->Slot(target), from.heap->Slot(origin), to.heap->Increment());
            to.heap->Record(target) = record;
        }
        destinationDone += run;
        sourceDone += run;
    }
}

}
