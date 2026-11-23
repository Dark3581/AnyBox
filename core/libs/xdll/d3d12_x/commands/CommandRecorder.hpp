#pragma once

#include "CopyLayout.hpp"
#include "IndirectCommands.hpp"
#include "TextureDesc.hpp"
#include "VulkanDevice.hpp"

#include <windows.h>
#include <d3d12.h>

#include <string>
#include <vector>

namespace abx::d3d12x {

struct TextureRef {
    VkImage image = VK_NULL_HANDLE;
    D3D12_RESOURCE_DESC desc{};
    TextureImage layout;
};

struct BufferRef {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
};

struct AttachmentRef {
    VkImageView view = VK_NULL_HANDLE;
    VkExtent2D extent{};
    std::uint32_t layers = 1;
    VkImageAspectFlags aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    D3D12_DSV_FLAGS readOnly = D3D12_DSV_FLAG_NONE;
};

struct VertexBufferRef {
    BufferRef buffer;
    UINT stride = 0;
};

std::optional<VkPrimitiveTopology> VulkanTopology(D3D_PRIMITIVE_TOPOLOGY topology);
constexpr D3D_PRIMITIVE_TOPOLOGY kConsoleRectangleList = static_cast<D3D_PRIMITIVE_TOPOLOGY>(17);
std::optional<VkViewport> VulkanViewport(const D3D12_VIEWPORT& viewport);

class CommandRecorder {
public:
    CommandRecorder(const VulkanDevice& device, VkCommandBuffer commands) : device_(device), commands_(commands) {}

    bool InitializeTexture(const TextureRef& texture, D3D12_RESOURCE_STATES initial, std::string& error);
    bool Transition(const TextureRef& texture, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, std::string& error);
    bool BufferTransition(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, std::string& error);
    void UnorderedAccessBarrier();
    bool ClearRenderTarget(const AttachmentRef& target, const float color[4], const std::vector<D3D12_RECT>& rects, std::string& error);
    bool ClearDepthStencil(const AttachmentRef& target, D3D12_CLEAR_FLAGS flags, float depth, std::uint8_t stencil, const std::vector<D3D12_RECT>& rects, std::string& error);
    bool CopyBufferRegion(const BufferRef& destination, UINT64 destinationOffset, const BufferRef& source, UINT64 sourceOffset, UINT64 bytes, std::string& error);
    bool CopyBufferToTexture(const TextureRef& destination, D3D12_RESOURCE_STATES state, UINT subresource, UINT x, UINT y, UINT z, const BufferRef& source,
                             const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint, const D3D12_BOX* box, std::string& error);
    bool CopyTextureToFootprint(const BufferRef& destination, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint, UINT x, UINT y, UINT z, const TextureRef& source,
                                D3D12_RESOURCE_STATES state, UINT subresource, const D3D12_BOX* box, std::string& error);
    bool CopyTextureToBuffer(const BufferRef& destination, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint, const TextureRef& source, D3D12_RESOURCE_STATES state,
                             UINT subresource, const D3D12_BOX* box, std::string& error);
    bool CopyTexture(const TextureRef& destination, D3D12_RESOURCE_STATES destinationState, UINT destinationSubresource, UINT x, UINT y, UINT z, const TextureRef& source,
                     D3D12_RESOURCE_STATES sourceState, UINT sourceSubresource, const D3D12_BOX* box, std::string& error);
    bool ResolveSubresource(const TextureRef& destination, D3D12_RESOURCE_STATES destinationState, UINT destinationSubresource, const TextureRef& source,
                            D3D12_RESOURCE_STATES sourceState, UINT sourceSubresource, std::string& error);

    bool BeginRendering(const std::vector<AttachmentRef>& colors, const AttachmentRef* depthStencil, VkExtent2D extent, std::uint32_t layers, std::string& error);
    void EndRendering();
    bool SetViewports(const std::vector<D3D12_VIEWPORT>& viewports, std::string& error);
    bool SetScissors(const std::vector<D3D12_RECT>& rects, std::string& error);
    bool SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY topology, bool stripCut, std::string& error);
    void SetPolygonMode(VkPolygonMode mode);
    void SetBlendFactor(const float factor[4]);
    void SetStencilReference(UINT reference);
    bool SetVertexBuffers(UINT firstSlot, const std::vector<VertexBufferRef>& buffers, std::string& error);
    bool SetIndexBuffer(const BufferRef& buffer, DXGI_FORMAT format, std::string& error);
    void BindPipeline(VkPipelineBindPoint point, VkPipeline pipeline);
    bool SetPushConstants(const void* data, std::uint32_t bytes, std::string& error);
    void BindTitleSet(VkPipelineBindPoint point, VkDescriptorSet set);
    void Draw(UINT vertices, UINT instances, UINT firstVertex, UINT firstInstance);
    void DrawIndexed(UINT indices, UINT instances, UINT firstIndex, INT baseVertex, UINT firstInstance);
    void Dispatch(UINT x, UINT y, UINT z);
    bool ExecuteIndirect(const IndirectSignature& signature, UINT maxCount, const BufferRef& arguments, UINT64 argumentOffset, const BufferRef* count, UINT64 countOffset,
                         std::string& error);

private:
    bool LayoutFor(const TextureRef& texture, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_STATES expected, VkImageLayout& layout, std::string& error) const;

    const VulkanDevice& device_;
    VkCommandBuffer commands_;
};

}
