#include "BindlessSet.hpp"

#include <algorithm>
#include <cstring>
#include <map>

namespace abx::d3d12x {

namespace {

VkFormat FallbackFormat(const BindingSpec& spec) {
    if (spec.format == ImageFormat::R32ui) {
        return VK_FORMAT_R32_UINT;
    }
    switch (spec.component) {
    case ImageComponent::Uint: return VK_FORMAT_R32G32B32A32_UINT;
    case ImageComponent::Sint: return VK_FORMAT_R32G32B32A32_SINT;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

VkImageType ImageType(ImageDim dim) {
    switch (dim) {
    case ImageDim::Dim1D: return VK_IMAGE_TYPE_1D;
    case ImageDim::Dim3D: return VK_IMAGE_TYPE_3D;
    default: return VK_IMAGE_TYPE_2D;
    }
}

VkImageViewType ViewType(ImageDim dim, bool arrayed) {
    switch (dim) {
    case ImageDim::Dim1D: return arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    case ImageDim::Dim3D: return VK_IMAGE_VIEW_TYPE_3D;
    default: return arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}

VkDescriptorType DescriptorTypeOf(DescriptorKind kind) {
    switch (kind) {
    case DescriptorKind::SampledImage: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case DescriptorKind::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
    case DescriptorKind::StorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case DescriptorKind::StorageImage: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}

const BindingSpec* FindBinding(std::uint32_t binding) {
    for (const BindingSpec& spec : TitleBindings()) {
        if (spec.binding == binding) {
            return &spec;
        }
    }
    return nullptr;
}

}

bool CreateHostBuffer(const VulkanDevice& device, VkDeviceSize size, VkBufferUsageFlags usage, DeviceBuffer& out, std::string& error) {
    const DeviceFunctions& vk = device.Vk();
    const bool address = device.BufferDeviceAddress() && (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
    if (!device.BufferDeviceAddress()) {
        usage &= ~static_cast<VkBufferUsageFlags>(VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    }
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vk.createBuffer(device.Handle(), &info, nullptr, &out.buffer) != VK_SUCCESS) {
        out.buffer = VK_NULL_HANDLE;
        error = "vkCreateBuffer failed";
        return false;
    }
    VkMemoryRequirements requirements{};
    vk.getBufferMemoryRequirements(device.Handle(), out.buffer, &requirements);
    const std::optional<std::uint32_t> type = device.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!type) {
        DestroyBuffer(device, out);
        error = "no host-visible coherent memory for a buffer";
        return false;
    }
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, address ? &flags : nullptr};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = *type;
    if (vk.allocateMemory(device.Handle(), &allocate, nullptr, &out.memory) != VK_SUCCESS) {
        out.memory = VK_NULL_HANDLE;
        DestroyBuffer(device, out);
        error = "vkAllocateMemory failed for a buffer";
        return false;
    }
    if (vk.bindBufferMemory(device.Handle(), out.buffer, out.memory, 0) != VK_SUCCESS ||
        vk.mapMemory(device.Handle(), out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped) != VK_SUCCESS) {
        out.mapped = nullptr;
        DestroyBuffer(device, out);
        error = "cannot bind or map a buffer";
        return false;
    }
    out.size = size;
    if (address) {
        VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        addressInfo.buffer = out.buffer;
        out.address = vk.getBufferDeviceAddress(device.Handle(), &addressInfo);
    }
    return true;
}

void DestroyBuffer(const VulkanDevice& device, DeviceBuffer& buffer) {
    const DeviceFunctions& vk = device.Vk();
    if (buffer.mapped != nullptr) {
        vk.unmapMemory(device.Handle(), buffer.memory);
    }
    if (buffer.buffer != VK_NULL_HANDLE) {
        vk.destroyBuffer(device.Handle(), buffer.buffer, nullptr);
    }
    if (buffer.memory != VK_NULL_HANDLE) {
        vk.freeMemory(device.Handle(), buffer.memory, nullptr);
    }
    buffer = DeviceBuffer{};
}

std::unique_ptr<BindlessSet> BindlessSet::Create(const VulkanDevice& device, std::string& error) {
    std::unique_ptr<BindlessSet> set(new BindlessSet(device));
    if (!set->Initialize(error)) {
        return nullptr;
    }
    return set;
}

BindlessSet::~BindlessSet() {
    const DeviceFunctions& vk = device_.Vk();
    const VkDevice device = device_.Handle();
    DestroyBuffer(device_, tableBuffer_);
    for (DeviceBuffer& retired : retiredTables_) DestroyBuffer(device_, retired);
    for (Fallback& fallback : fallbacks_) {
        if (fallback.view != VK_NULL_HANDLE) vk.destroyImageView(device, fallback.view, nullptr);
        if (fallback.image != VK_NULL_HANDLE) vk.destroyImage(device, fallback.image, nullptr);
        if (fallback.memory != VK_NULL_HANDLE) vk.freeMemory(device, fallback.memory, nullptr);
    }
    if (fallbackSampler_ != VK_NULL_HANDLE) vk.destroySampler(device, fallbackSampler_, nullptr);
    if (pool_ != VK_NULL_HANDLE) vk.destroyDescriptorPool(device, pool_, nullptr);
}

BindlessSet::Fallback* BindlessSet::FallbackFor(const BindingSpec& spec, std::string& error) {
    const VkFormat format = FallbackFormat(spec);
    for (Fallback& fallback : fallbacks_) {
        if (fallback.dim == spec.dim && fallback.arrayed == spec.arrayed && fallback.format == format) {
            return &fallback;
        }
    }
    const DeviceFunctions& vk = device_.Vk();
    Fallback fallback;
    fallback.dim = spec.dim;
    fallback.arrayed = spec.arrayed;
    fallback.format = format;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = ImageType(spec.dim);
    info.format = format;
    info.extent = {1, 1, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk.createImage(device_.Handle(), &info, nullptr, &fallback.image) != VK_SUCCESS) {
        error = "vkCreateImage failed for a fallback image";
        return nullptr;
    }
    fallbacks_.push_back(fallback);
    Fallback& stored = fallbacks_.back();
    VkMemoryRequirements requirements{};
    vk.getImageMemoryRequirements(device_.Handle(), stored.image, &requirements);
    const std::optional<std::uint32_t> type = device_.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryPriorityAllocateInfoEXT priority{VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT};
    priority.priority = 0.75f;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    if (device_.PageableMemory()) allocate.pNext = &priority;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type.value_or(0);
    if (!type || vk.allocateMemory(device_.Handle(), &allocate, nullptr, &stored.memory) != VK_SUCCESS ||
        vk.bindImageMemory(device_.Handle(), stored.image, stored.memory, 0) != VK_SUCCESS) {
        error = "cannot allocate memory for a fallback image";
        return nullptr;
    }
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = stored.image;
    viewInfo.viewType = ViewType(spec.dim, spec.arrayed);
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vk.createImageView(device_.Handle(), &viewInfo, nullptr, &stored.view) != VK_SUCCESS) {
        stored.view = VK_NULL_HANDLE;
        error = "vkCreateImageView failed for a fallback image";
        return nullptr;
    }
    return &stored;
}

bool BindlessSet::Initialize(std::string& error) {
    const DeviceFunctions& vk = device_.Vk();
    std::map<VkDescriptorType, std::uint32_t> counts;
    for (const BindingSpec& spec : TitleBindings()) {
        counts[DescriptorTypeOf(spec.kind)] += spec.count;
    }
    std::vector<VkDescriptorPoolSize> sizes;
    for (const auto& [type, count] : counts) {
        sizes.push_back({type, count});
    }
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    if (vk.createDescriptorPool(device_.Handle(), &poolInfo, nullptr, &pool_) != VK_SUCCESS) {
        pool_ = VK_NULL_HANDLE;
        error = "vkCreateDescriptorPool failed";
        return false;
    }
    const VkDescriptorSetLayout layout = device_.TitleSetLayout();
    VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setInfo.descriptorPool = pool_;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &layout;
    if (vk.allocateDescriptorSets(device_.Handle(), &setInfo, &set_) != VK_SUCCESS) {
        error = "vkAllocateDescriptorSets failed";
        return false;
    }
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (vk.createSampler(device_.Handle(), &samplerInfo, nullptr, &fallbackSampler_) != VK_SUCCESS) {
        fallbackSampler_ = VK_NULL_HANDLE;
        error = "vkCreateSampler failed";
        return false;
    }
    fallbacks_.reserve(TitleBindings().size());
    for (const BindingSpec& spec : TitleBindings()) {
        if ((spec.kind == DescriptorKind::SampledImage || spec.kind == DescriptorKind::StorageImage) && FallbackFor(spec, error) == nullptr) {
            return false;
        }
    }
    if (!device_.Submit([this, &vk](VkCommandBuffer commands) {
            std::vector<VkImageMemoryBarrier> toTransfer, toGeneral;
            for (const Fallback& fallback : fallbacks_) {
                VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = fallback.image;
                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toTransfer.push_back(barrier);
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                toGeneral.push_back(barrier);
            }
            vk.cmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                  static_cast<std::uint32_t>(toTransfer.size()), toTransfer.data());
            const VkClearColorValue zero{};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            for (const Fallback& fallback : fallbacks_) {
                vk.cmdClearColorImage(commands, fallback.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
            }
            vk.cmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                                  static_cast<std::uint32_t>(toGeneral.size()), toGeneral.data());
        },
        error)) {
        return false;
    }
    for (const BindingSpec& spec : TitleBindings()) {
        if (spec.kind == DescriptorKind::Sampler) {
            if (!WriteSampler(0, fallbackSampler_, error)) return false;
        } else if (spec.kind != DescriptorKind::StorageBuffer) {
            if (!WriteImage(spec.binding, 0, FallbackFor(spec, error)->view, error)) return false;
        }
    }
    return Upload(error);
}

void BindlessSet::WriteTableBinding() {
    VkDescriptorBufferInfo info{tableBuffer_.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set_;
    write.dstBinding = 2;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    device_.Vk().updateDescriptorSets(device_.Handle(), 1, &write, 0, nullptr);
}

bool BindlessSet::Upload(std::string& error) {
    if (uploaded_ == table_.Generation() && tableBuffer_.buffer != VK_NULL_HANDLE) {
        return true;
    }
    const std::vector<std::uint32_t>& words = table_.Words();
    const VkDeviceSize bytes = words.size() * sizeof(std::uint32_t);
    if (tableBuffer_.size < bytes) {
        DeviceBuffer grown;
        if (!CreateHostBuffer(device_, std::max<VkDeviceSize>(bytes, tableBuffer_.size * 2), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, grown, error)) {
            return false;
        }
        if (tableBuffer_.buffer != VK_NULL_HANDLE) retiredTables_.push_back(tableBuffer_);
        tableBuffer_ = grown;
        WriteTableBinding();
    }
    std::memcpy(tableBuffer_.mapped, words.data(), static_cast<std::size_t>(bytes));
    uploaded_ = table_.Generation();
    return true;
}

bool BindlessSet::WriteImage(std::uint32_t binding, std::uint32_t index, VkImageView view, std::string& error) {
    const BindingSpec* spec = FindBinding(binding);
    if (spec == nullptr || (spec->kind != DescriptorKind::SampledImage && spec->kind != DescriptorKind::StorageImage) || index >= spec->count) {
        error = "binding " + std::to_string(binding) + " has no image slot " + std::to_string(index);
        return false;
    }
    VkDescriptorImageInfo info{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set_;
    write.dstBinding = binding;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = DescriptorTypeOf(spec->kind);
    write.pImageInfo = &info;
    device_.Vk().updateDescriptorSets(device_.Handle(), 1, &write, 0, nullptr);
    return true;
}

bool BindlessSet::WriteSampler(std::uint32_t index, VkSampler sampler, std::string& error) {
    const BindingSpec* spec = FindBinding(1);
    if (spec == nullptr || index >= spec->count) {
        error = "no sampler slot " + std::to_string(index);
        return false;
    }
    VkDescriptorImageInfo info{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set_;
    write.dstBinding = 1;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &info;
    device_.Vk().updateDescriptorSets(device_.Handle(), 1, &write, 0, nullptr);
    return true;
}

}
