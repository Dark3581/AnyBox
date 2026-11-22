#include "CommandRecorder.hpp"

#include "ResourceState.hpp"

#include <algorithm>
#include <cmath>

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

std::optional<VkPrimitiveTopology> VulkanTopology(D3D_PRIMITIVE_TOPOLOGY topology) {
    switch (topology) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    default: return std::nullopt;
    }
}

std::optional<VkViewport> VulkanViewport(const D3D12_VIEWPORT& viewport) {
    const float values[6] = {viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height, viewport.MinDepth, viewport.MaxDepth};
    for (const float value : values) {
        if (!std::isfinite(value)) {
            return std::nullopt;
        }
    }
    if (viewport.Width <= 0.0f || viewport.Height <= 0.0f || viewport.MinDepth < 0.0f || viewport.MinDepth > 1.0f || viewport.MaxDepth < 0.0f || viewport.MaxDepth > 1.0f) {
        return std::nullopt;
    }
    return VkViewport{viewport.TopLeftX, viewport.TopLeftY + viewport.Height, viewport.Width, -viewport.Height, viewport.MinDepth, viewport.MaxDepth};
}

bool CommandRecorder::BeginRendering(const std::vector<AttachmentRef>& colors, const AttachmentRef* depthStencil, VkExtent2D extent, std::uint32_t layers, std::string& error) {
    if (colors.size() > 8 || extent.width == 0 || extent.height == 0 || layers == 0) {
        error = "a render pass has more than 8 color targets or an empty area";
        return false;
    }
    std::vector<VkRenderingAttachmentInfo> attachments(colors.size(), VkRenderingAttachmentInfo{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO});
    for (std::size_t index = 0; index < colors.size(); ++index) {
        attachments[index].imageView = colors[index].view;
        attachments[index].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments[index].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[index].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, extent};
    rendering.layerCount = layers;
    rendering.colorAttachmentCount = static_cast<std::uint32_t>(attachments.size());
    rendering.pColorAttachments = attachments.empty() ? nullptr : attachments.data();
    if (depthStencil != nullptr) {
        const UINT readOnly = static_cast<UINT>(depthStencil->readOnly);
        const bool hasStencil = (depthStencil->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
        const bool depthReadOnly = (readOnly & D3D12_DSV_FLAG_READ_ONLY_DEPTH) != 0 || (depthStencil->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) == 0;
        const bool stencilReadOnly = (readOnly & D3D12_DSV_FLAG_READ_ONLY_STENCIL) != 0 || !hasStencil;
        if (readOnly != 0 && depthReadOnly != stencilReadOnly) {
            error = "a depth-stencil pass that is read-only for only one of its planes is not supported";
            return false;
        }
        depth.imageView = depthStencil->view;
        depth.imageLayout = readOnly != 0 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = readOnly != 0 ? VK_ATTACHMENT_STORE_OP_NONE : VK_ATTACHMENT_STORE_OP_STORE;
        stencil = depth;
        if ((depthStencil->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0) rendering.pDepthAttachment = &depth;
        if ((depthStencil->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) rendering.pStencilAttachment = &stencil;
    }
    device_.Vk().cmdBeginRendering(commands_, &rendering);
    return true;
}

void CommandRecorder::EndRendering() {
    device_.Vk().cmdEndRendering(commands_);
}

bool CommandRecorder::SetViewports(const std::vector<D3D12_VIEWPORT>& viewports, std::string& error) {
    if (viewports.empty() || viewports.size() > D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) {
        error = std::to_string(viewports.size()) + " viewports are not between 1 and 16";
        return false;
    }
    std::vector<VkViewport> converted;
    for (const D3D12_VIEWPORT& viewport : viewports) {
        const std::optional<VkViewport> vulkan = VulkanViewport(viewport);
        if (!vulkan) {
            error = "a viewport is empty, not finite or has a depth range outside 0 to 1";
            return false;
        }
        converted.push_back(*vulkan);
    }
    device_.Vk().cmdSetViewportWithCount(commands_, static_cast<std::uint32_t>(converted.size()), converted.data());
    return true;
}

bool CommandRecorder::SetScissors(const std::vector<D3D12_RECT>& rects, std::string& error) {
    if (rects.empty() || rects.size() > D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) {
        error = std::to_string(rects.size()) + " scissor rectangles are not between 1 and 16";
        return false;
    }
    std::vector<VkRect2D> converted;
    for (const D3D12_RECT& rect : rects) {
        const LONG left = (std::max)(rect.left, 0L);
        const LONG top = (std::max)(rect.top, 0L);
        const LONG right = (std::max)(rect.right, left);
        const LONG bottom = (std::max)(rect.bottom, top);
        converted.push_back({{left, top}, {static_cast<std::uint32_t>(right - left), static_cast<std::uint32_t>(bottom - top)}});
    }
    device_.Vk().cmdSetScissorWithCount(commands_, static_cast<std::uint32_t>(converted.size()), converted.data());
    return true;
}

bool CommandRecorder::SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY topology, bool stripCut, std::string& error) {
    const std::optional<VkPrimitiveTopology> vulkan = VulkanTopology(topology);
    if (!vulkan) {
        error = "primitive topology " + std::to_string(static_cast<int>(topology)) + " is not supported";
        return false;
    }
    const bool strip = *vulkan == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || *vulkan == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                       *vulkan == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY || *vulkan == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    device_.Vk().cmdSetPrimitiveTopology(commands_, *vulkan);
    device_.Vk().cmdSetPrimitiveRestartEnable(commands_, strip && stripCut ? VK_TRUE : VK_FALSE);
    return true;
}

void CommandRecorder::SetBlendFactor(const float factor[4]) {
    device_.Vk().cmdSetBlendConstants(commands_, factor);
}

void CommandRecorder::SetStencilReference(UINT reference) {
    device_.Vk().cmdSetStencilReference(commands_, VK_STENCIL_FACE_FRONT_AND_BACK, reference & 0xFFu);
}

bool CommandRecorder::SetVertexBuffers(UINT firstSlot, const std::vector<VertexBufferRef>& buffers, std::string& error) {
    if (firstSlot >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT || buffers.size() > D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - firstSlot) {
        error = "vertex buffer slots " + std::to_string(firstSlot) + "+" + std::to_string(buffers.size()) + " pass slot 31";
        return false;
    }
    if (buffers.empty()) {
        return true;
    }
    std::vector<VkBuffer> handles;
    std::vector<VkDeviceSize> offsets;
    std::vector<VkDeviceSize> sizes;
    std::vector<VkDeviceSize> strides;
    for (const VertexBufferRef& view : buffers) {
        if (view.buffer.buffer == VK_NULL_HANDLE || view.buffer.size == 0) {
            error = "an empty vertex buffer view is not supported";
            return false;
        }
        handles.push_back(view.buffer.buffer);
        offsets.push_back(view.buffer.offset);
        sizes.push_back(view.buffer.size);
        strides.push_back(view.stride);
    }
    device_.Vk().cmdBindVertexBuffers2(commands_, firstSlot, static_cast<std::uint32_t>(handles.size()), handles.data(), offsets.data(), sizes.data(), strides.data());
    return true;
}

bool CommandRecorder::SetIndexBuffer(const BufferRef& buffer, DXGI_FORMAT format, std::string& error) {
    VkIndexType type = VK_INDEX_TYPE_UINT16;
    if (format == DXGI_FORMAT_R32_UINT) {
        type = VK_INDEX_TYPE_UINT32;
    } else if (format != DXGI_FORMAT_R16_UINT) {
        error = "index format " + std::to_string(static_cast<int>(format)) + " is not R16_UINT or R32_UINT";
        return false;
    }
    if (buffer.buffer == VK_NULL_HANDLE || buffer.offset % (type == VK_INDEX_TYPE_UINT32 ? 4 : 2) != 0) {
        error = "the index buffer is missing or misaligned";
        return false;
    }
    device_.Vk().cmdBindIndexBuffer(commands_, buffer.buffer, buffer.offset, type);
    return true;
}

void CommandRecorder::BindPipeline(VkPipelineBindPoint point, VkPipeline pipeline) {
    device_.Vk().cmdBindPipeline(commands_, point, pipeline);
}

bool CommandRecorder::SetPushConstants(const void* data, std::uint32_t bytes, std::string& error) {
    if (data == nullptr || bytes == 0 || bytes > 144 || bytes % 4 != 0) {
        error = std::to_string(bytes) + " push constant bytes are not a whole number of words up to 144";
        return false;
    }
    device_.Vk().cmdPushConstants(commands_, device_.TitlePipelineLayout(), VK_SHADER_STAGE_ALL, 0, bytes, data);
    return true;
}

void CommandRecorder::Draw(UINT vertices, UINT instances, UINT firstVertex, UINT firstInstance) {
    device_.Vk().cmdDraw(commands_, vertices, instances, firstVertex, firstInstance);
}

void CommandRecorder::DrawIndexed(UINT indices, UINT instances, UINT firstIndex, INT baseVertex, UINT firstInstance) {
    device_.Vk().cmdDrawIndexed(commands_, indices, instances, firstIndex, baseVertex, firstInstance);
}

void CommandRecorder::Dispatch(UINT x, UINT y, UINT z) {
    device_.Vk().cmdDispatch(commands_, x, y, z);
}

bool CommandRecorder::ExecuteIndirect(const IndirectSignature& signature, UINT maxCount, const BufferRef& arguments, UINT64 argumentOffset, const BufferRef* count,
                                      UINT64 countOffset, std::string& error) {
    if (maxCount == 0) {
        return true;
    }
    const std::uint64_t span = static_cast<std::uint64_t>(signature.stride) * (maxCount - 1) + IndirectArgumentBytes(signature.kind);
    if (argumentOffset % 4 != 0 || argumentOffset > arguments.size || span > arguments.size - argumentOffset) {
        error = "the indirect arguments are misaligned or run past their buffer";
        return false;
    }
    if (count != nullptr && (countOffset % 4 != 0 || countOffset > count->size || count->size - countOffset < 4)) {
        error = "the indirect count is misaligned or outside its buffer";
        return false;
    }
    const DeviceFunctions& vk = device_.Vk();
    const VkDeviceSize offset = arguments.offset + argumentOffset;
    if (signature.kind == IndirectKind::Dispatch) {
        if (count != nullptr) {
            error = "an indirect dispatch with a count buffer is not supported";
            return false;
        }
        for (UINT index = 0; index < maxCount; ++index) {
            vk.cmdDispatchIndirect(commands_, arguments.buffer, offset + static_cast<VkDeviceSize>(index) * signature.stride);
        }
        return true;
    }
    const bool indexed = signature.kind == IndirectKind::DrawIndexed;
    if (count != nullptr) {
        if (!device_.HasFeature("drawIndirectCount")) {
            error = "an indirect draw with a count buffer needs drawIndirectCount, which the device does not offer";
            return false;
        }
        const VkDeviceSize countAt = count->offset + countOffset;
        if (indexed) {
            vk.cmdDrawIndexedIndirectCount(commands_, arguments.buffer, offset, count->buffer, countAt, maxCount, signature.stride);
        } else {
            vk.cmdDrawIndirectCount(commands_, arguments.buffer, offset, count->buffer, countAt, maxCount, signature.stride);
        }
        return true;
    }
    const bool multiDraw = device_.HasFeature("multiDrawIndirect");
    const UINT calls = multiDraw ? 1 : maxCount;
    const UINT perCall = multiDraw ? maxCount : 1;
    for (UINT index = 0; index < calls; ++index) {
        const VkDeviceSize at = offset + static_cast<VkDeviceSize>(index) * signature.stride;
        if (indexed) {
            vk.cmdDrawIndexedIndirect(commands_, arguments.buffer, at, perCall, signature.stride);
        } else {
            vk.cmdDrawIndirect(commands_, arguments.buffer, at, perCall, signature.stride);
        }
    }
    return true;
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
    const std::optional<Subresource> to = SplitSubresource(destination.desc, destinationSubresource, destination.layout.info.mipLevels);
    const std::optional<Subresource> from = SplitSubresource(source.desc, sourceSubresource, source.layout.info.mipLevels);
    const std::optional<VkImageAspectFlags> toAspect = to ? PlaneAspect(destination.layout.format.aspects, to->plane) : std::nullopt;
    const std::optional<VkImageAspectFlags> fromAspect = from ? PlaneAspect(source.layout.format.aspects, from->plane) : std::nullopt;
    if (!toAspect || !fromAspect) {
        error = "a copy subresource is outside its texture or names a plane it does not have";
        return false;
    }
    if (*toAspect != *fromAspect || (*toAspect == VK_IMAGE_ASPECT_COLOR_BIT && (TexelBytes(destination) == 0 || TexelBytes(destination) != TexelBytes(source))) ||
        (*toAspect != VK_IMAGE_ASPECT_COLOR_BIT && destination.layout.info.format != source.layout.info.format)) {
        error = "the two textures' formats or planes are not copy compatible";
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
    copy.srcSubresource = {*fromAspect, from->mip, source.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 0u : from->layer, 1};
    copy.dstSubresource = {*toAspect, to->mip, destination.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 0u : to->layer, 1};
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
