#include "CommandRecorder.hpp"

#include "ResourceState.hpp"

#include <algorithm>

namespace abx::d3d12x {

namespace {

bool ToRects(const std::vector<D3D12_RECT>& rects, const AttachmentRef& target, std::vector<VkClearRect>& out, std::string& error) {
    for (const D3D12_RECT& rect : rects) {
        const LONG right = (std::min)(rect.right, static_cast<LONG>(target.extent.width));
        const LONG bottom = (std::min)(rect.bottom, static_cast<LONG>(target.extent.height));
        const LONG left = (std::max)(rect.left, 0L);
        const LONG top = (std::max)(rect.top, 0L);
        if (left > right || top > bottom) {
            error = "a clear rectangle is inverted";
            return false;
        }
        if (left == right || top == bottom) {
            continue;
        }
        VkClearRect clear{};
        clear.rect.offset = {left, top};
        clear.rect.extent = {static_cast<std::uint32_t>(right - left), static_cast<std::uint32_t>(bottom - top)};
        clear.baseArrayLayer = 0;
        clear.layerCount = target.layers;
        out.push_back(clear);
    }
    return true;
}

UINT64 TexelBytes(const TextureRef& texture) {
    const std::optional<CopyBlock> block = CopyBlockOf(texture.desc.Format);
    return block ? block->bytes : 0;
}

}

bool CommandRecorder::LayoutFor(const TextureRef& texture, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_STATES expected, VkImageLayout& layout, std::string& error) const {
    if (state != D3D12_RESOURCE_STATE_COMMON && (static_cast<UINT>(state) & static_cast<UINT>(expected)) == 0) {
        error = "the texture is in state " + std::to_string(static_cast<UINT>(state)) + ", not in state " + std::to_string(static_cast<UINT>(expected)) + " or COMMON";
        return false;
    }
    const std::optional<ResourceUsage> usage = UsageOfState(state, (texture.layout.format.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0, error);
    if (!usage) {
        return false;
    }
    layout = usage->layout;
    return true;
}

bool CommandRecorder::InitializeTexture(const TextureRef& texture, D3D12_RESOURCE_STATES initial, std::string& error) {
    const std::optional<ResourceUsage> usage = UsageOfState(initial, (texture.layout.format.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0, error);
    if (!usage) {
        return false;
    }
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    barrier.dstStageMask = usage->stages;
    barrier.dstAccessMask = usage->access;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = usage->layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image;
    barrier.subresourceRange = {texture.layout.format.aspects, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    device_.Vk().cmdPipelineBarrier2(commands_, &dependency);
    return true;
}

bool CommandRecorder::Transition(const TextureRef& texture, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, std::string& error) {
    std::optional<VkImageMemoryBarrier2> barrier = ImageTransition(before, after, (texture.layout.format.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0, error);
    if (!barrier) {
        return false;
    }
    barrier->image = texture.image;
    barrier->subresourceRange = {texture.layout.format.aspects, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &*barrier;
    device_.Vk().cmdPipelineBarrier2(commands_, &dependency);
    return true;
}

bool CommandRecorder::BufferTransition(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, std::string& error) {
    const std::optional<VkMemoryBarrier2> barrier = abx::d3d12x::BufferTransition(before, after, error);
    if (!barrier) {
        return false;
    }
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &*barrier;
    device_.Vk().cmdPipelineBarrier2(commands_, &dependency);
    return true;
}

void CommandRecorder::UnorderedAccessBarrier() {
    const VkMemoryBarrier2 barrier = abx::d3d12x::UnorderedAccessBarrier();
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    device_.Vk().cmdPipelineBarrier2(commands_, &dependency);
}

bool CommandRecorder::ClearRenderTarget(const AttachmentRef& target, const float color[4], const std::vector<D3D12_RECT>& rects, std::string& error) {
    std::vector<VkClearRect> clearRects;
    if (!ToRects(rects, target, clearRects, error)) {
        return false;
    }
    if (!rects.empty() && clearRects.empty()) {
        return true;
    }
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = target.view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = rects.empty() ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    std::copy(color, color + 4, attachment.clearValue.color.float32);
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, target.extent};
    rendering.layerCount = target.layers;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    const DeviceFunctions& vk = device_.Vk();
    vk.cmdBeginRendering(commands_, &rendering);
    if (!clearRects.empty()) {
        VkClearAttachment clear{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
        std::copy(color, color + 4, clear.clearValue.color.float32);
        vk.cmdClearAttachments(commands_, 1, &clear, static_cast<std::uint32_t>(clearRects.size()), clearRects.data());
    }
    vk.cmdEndRendering(commands_);
    return true;
}

bool CommandRecorder::ClearDepthStencil(const AttachmentRef& target, D3D12_CLEAR_FLAGS flags, float depth, std::uint8_t stencil, const std::vector<D3D12_RECT>& rects,
                                        std::string& error) {
    const UINT bits = static_cast<UINT>(flags);
    if (bits == 0 || (bits & ~static_cast<UINT>(D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL)) != 0) {
        error = "clear flags " + std::to_string(bits) + " are not depth and/or stencil";
        return false;
    }
    if (!(depth >= 0.0f && depth <= 1.0f)) {
        error = "the depth clear value is outside 0 to 1";
        return false;
    }
    VkImageAspectFlags cleared = 0;
    if ((bits & D3D12_CLEAR_FLAG_DEPTH) != 0 && (target.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) cleared |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if ((bits & D3D12_CLEAR_FLAG_STENCIL) != 0 && (target.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) cleared |= VK_IMAGE_ASPECT_STENCIL_BIT;
    std::vector<VkClearRect> clearRects;
    if (!ToRects(rects, target, clearRects, error)) {
        return false;
    }
    if (cleared == 0 || (!rects.empty() && clearRects.empty())) {
        return true;
    }
    const bool full = rects.empty();
    VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAttachment.imageView = target.view;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = full && (cleared & VK_IMAGE_ASPECT_DEPTH_BIT) != 0 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.clearValue.depthStencil = {depth, stencil};
    VkRenderingAttachmentInfo stencilAttachment = depthAttachment;
    stencilAttachment.loadOp = full && (cleared & VK_IMAGE_ASPECT_STENCIL_BIT) != 0 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, target.extent};
    rendering.layerCount = target.layers;
    if ((target.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) rendering.pDepthAttachment = &depthAttachment;
    if ((target.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) rendering.pStencilAttachment = &stencilAttachment;
    const DeviceFunctions& vk = device_.Vk();
    vk.cmdBeginRendering(commands_, &rendering);
    if (!clearRects.empty()) {
        VkClearAttachment clear{cleared, 0, {}};
        clear.clearValue.depthStencil = {depth, stencil};
        vk.cmdClearAttachments(commands_, 1, &clear, static_cast<std::uint32_t>(clearRects.size()), clearRects.data());
    }
    vk.cmdEndRendering(commands_);
    return true;
}

bool CommandRecorder::CopyBufferRegion(const BufferRef& destination, UINT64 destinationOffset, const BufferRef& source, UINT64 sourceOffset, UINT64 bytes, std::string& error) {
    if (bytes == 0 || destinationOffset > destination.size || bytes > destination.size - destinationOffset || sourceOffset > source.size || bytes > source.size - sourceOffset) {
        error = "the buffer copy is empty or runs past a buffer";
        return false;
    }
    const VkBufferCopy region{source.offset + sourceOffset, destination.offset + destinationOffset, bytes};
    device_.Vk().cmdCopyBuffer(commands_, source.buffer, destination.buffer, 1, &region);
    return true;
}

bool CommandRecorder::CopyBufferToTexture(const TextureRef& destination, D3D12_RESOURCE_STATES state, UINT subresource, UINT x, UINT y, UINT z, const BufferRef& source,
                                          const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint, const D3D12_BOX* box, std::string& error) {
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!LayoutFor(destination, state, D3D12_RESOURCE_STATE_COPY_DEST, layout, error)) {
        return false;
    }
    std::optional<VkBufferImageCopy> region = FootprintRegion(destination.desc, destination.layout.info.mipLevels, destination.layout.format.aspects, subresource, footprint, x, y, z, box, error);
    if (!region) {
        return false;
    }
    if (footprint.Offset > source.size) {
        error = "the footprint starts past the buffer";
        return false;
    }
    region->bufferOffset += source.offset;
    device_.Vk().cmdCopyBufferToImage(commands_, source.buffer, destination.image, layout, 1, &*region);
    return true;
}

bool CommandRecorder::CopyTextureToBuffer(const BufferRef& destination, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint, const TextureRef& source, D3D12_RESOURCE_STATES state,
                                          UINT subresource, const D3D12_BOX* box, std::string& error) {
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!LayoutFor(source, state, D3D12_RESOURCE_STATE_COPY_SOURCE, layout, error)) {
        return false;
    }
    D3D12_BOX sourceBox{};
    UINT x = 0;
    UINT y = 0;
    UINT z = 0;
    if (box != nullptr) {
        x = box->left;
        y = box->top;
        z = box->front;
        sourceBox = {0, 0, 0, box->right - box->left, box->bottom - box->top, box->back - box->front};
    }
    std::optional<VkBufferImageCopy> region = FootprintRegion(source.desc, source.layout.info.mipLevels, source.layout.format.aspects, subresource, footprint, x, y, z,
                                                              box != nullptr ? &sourceBox : nullptr, error);
    if (!region) {
        return false;
    }
    if (footprint.Offset > destination.size) {
        error = "the footprint starts past the buffer";
        return false;
    }
    region->bufferOffset += destination.offset;
    device_.Vk().cmdCopyImageToBuffer(commands_, source.image, layout, destination.buffer, 1, &*region);
    return true;
}

bool CommandRecorder::CopyTexture(const TextureRef& destination, D3D12_RESOURCE_STATES destinationState, UINT destinationSubresource, UINT x, UINT y, UINT z,
                                  const TextureRef& source, D3D12_RESOURCE_STATES sourceState, UINT sourceSubresource, const D3D12_BOX* box, std::string& error) {
    VkImageLayout destinationLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout sourceLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!LayoutFor(destination, destinationState, D3D12_RESOURCE_STATE_COPY_DEST, destinationLayout, error) ||
        !LayoutFor(source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceLayout, error)) {
        return false;
    }
    if (TexelBytes(destination) == 0 || TexelBytes(destination) != TexelBytes(source)) {
        error = "the two textures' formats are not copy compatible";
        return false;
    }
    const std::optional<Subresource> to = SplitSubresource(destination.desc, destinationSubresource, destination.layout.info.mipLevels);
    const std::optional<Subresource> from = SplitSubresource(source.desc, sourceSubresource, source.layout.info.mipLevels);
    if (!to || !from || to->plane != 0 || from->plane != 0) {
        error = "a copy subresource is outside its texture or names a second plane";
        return false;
    }
    const auto extentOf = [](const TextureRef& texture, std::uint32_t mip) {
        return VkExtent3D{MipExtent(texture.desc.Width, mip), texture.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1u : MipExtent(texture.desc.Height, mip),
                          texture.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? MipExtent(texture.desc.DepthOrArraySize, mip) : 1u};
    };
    const VkExtent3D sourceExtent = extentOf(source, from->mip);
    const VkExtent3D destinationExtent = extentOf(destination, to->mip);
    D3D12_BOX region = box != nullptr ? *box : D3D12_BOX{0, 0, 0, sourceExtent.width, sourceExtent.height, sourceExtent.depth};
    if (region.left >= region.right || region.top >= region.bottom || region.front >= region.back || region.right > sourceExtent.width || region.bottom > sourceExtent.height ||
        region.back > sourceExtent.depth || x + (region.right - region.left) > destinationExtent.width || y + (region.bottom - region.top) > destinationExtent.height ||
        z + (region.back - region.front) > destinationExtent.depth) {
        error = "the texture copy box is empty or outside one of the textures";
        return false;
    }
    VkImageCopy copy{};
    copy.srcSubresource = {source.layout.format.aspects, from->mip, source.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 0u : from->layer, 1};
    copy.dstSubresource = {destination.layout.format.aspects, to->mip, destination.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 0u : to->layer, 1};
    copy.srcOffset = {static_cast<std::int32_t>(region.left), static_cast<std::int32_t>(region.top), static_cast<std::int32_t>(region.front)};
    copy.dstOffset = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), static_cast<std::int32_t>(z)};
    copy.extent = {region.right - region.left, region.bottom - region.top, region.back - region.front};
    device_.Vk().cmdCopyImage(commands_, source.image, sourceLayout, destination.image, destinationLayout, 1, &copy);
    return true;
}

bool CommandRecorder::ResolveSubresource(const TextureRef& destination, D3D12_RESOURCE_STATES destinationState, UINT destinationSubresource, const TextureRef& source,
                                         D3D12_RESOURCE_STATES sourceState, UINT sourceSubresource, std::string& error) {
    VkImageLayout destinationLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout sourceLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!LayoutFor(destination, destinationState, D3D12_RESOURCE_STATE_RESOLVE_DEST, destinationLayout, error) ||
        !LayoutFor(source, sourceState, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, sourceLayout, error)) {
        return false;
    }
    if (source.layout.info.samples == VK_SAMPLE_COUNT_1_BIT || destination.layout.info.samples != VK_SAMPLE_COUNT_1_BIT || source.layout.info.format != destination.layout.info.format ||
        (source.layout.format.aspects & VK_IMAGE_ASPECT_COLOR_BIT) == 0) {
        error = "a resolve needs a multisampled color source and a single-sampled destination of the same format";
        return false;
    }
    const std::optional<Subresource> to = SplitSubresource(destination.desc, destinationSubresource, destination.layout.info.mipLevels);
    const std::optional<Subresource> from = SplitSubresource(source.desc, sourceSubresource, source.layout.info.mipLevels);
    if (!to || !from || to->plane != 0 || from->plane != 0 || MipExtent(destination.desc.Width, to->mip) != source.desc.Width ||
        MipExtent(destination.desc.Height, to->mip) != source.desc.Height) {
        error = "the resolve subresources are outside their textures or differ in size";
        return false;
    }
    VkImageResolve resolve{};
    resolve.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, from->mip, from->layer, 1};
    resolve.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, to->mip, to->layer, 1};
    resolve.extent = {static_cast<std::uint32_t>(source.desc.Width), source.desc.Height, 1};
    device_.Vk().cmdResolveImage(commands_, source.image, sourceLayout, destination.image, destinationLayout, 1, &resolve);
    return true;
}

}
