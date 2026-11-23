#include "CommandList.hpp"

#include "AbxApi.hpp"
#include "CopyLayout.hpp"
#include "ResourceState.hpp"

#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace abx::d3d12x {

namespace {

constexpr UINT kAllSubresources = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

[[noreturn]] void NotYet(const char* function) {
    AbxUnsupported(function, "not implemented yet: it needs descriptor heaps, root signatures or pipeline state objects");
}

}

HRESULT CommandList::Create(const ResourceContext& context, IUnknown* parent, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial,
                            REFIID riid, void** list, std::string& error) {
    if (list == nullptr) {
        error = "no output for the command list";
        return E_POINTER;
    }
    *list = nullptr;
    CommandList* created = new CommandList(context, parent, type);
    if (!created->Begin(allocator, initial, error)) {
        created->Release();
        return E_INVALIDARG;
    }
    const HRESULT result = created->QueryInterface(riid, list);
    created->Release();
    if (FAILED(result)) error = "the command list does not implement the requested interface";
    return result;
}

bool CommandList::Implements(REFIID riid) const {
    return DeviceChildBase::Implements(riid) || riid == __uuidof(ID3D12CommandList) || riid == __uuidof(ID3D12GraphicsCommandList);
}

bool CommandList::Begin(ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial, std::string& error) {
    CommandAllocator* pool = dynamic_cast<CommandAllocator*>(allocator);
    if (pool == nullptr || pool->Type() != type_) {
        error = "the command allocator is missing, foreign or of another type";
        return false;
    }
    if (initial != nullptr) {
        error = "an initial pipeline state is not supported yet";
        return false;
    }
    const VkCommandBuffer commands = pool->Next(error);
    if (commands == VK_NULL_HANDLE) {
        return false;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (context_.device.Vk().beginCommandBuffer(commands, &begin) != VK_SUCCESS) {
        error = "vkBeginCommandBuffer failed";
        return false;
    }
    commands_ = commands;
    closed_ = false;
    states_.clear();
    return true;
}

HRESULT STDMETHODCALLTYPE CommandList::Close() {
    if (closed_) {
        return E_FAIL;
    }
    closed_ = true;
    VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    host.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &host;
    context_.device.Vk().cmdPipelineBarrier2(commands_, &dependency);
    return context_.device.Vk().endCommandBuffer(commands_) == VK_SUCCESS ? S_OK : E_FAIL;
}

HRESULT STDMETHODCALLTYPE CommandList::Reset(ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial) {
    if (!closed_) {
        return E_FAIL;
    }
    std::string error;
    return Begin(allocator, initial, error) ? S_OK : E_INVALIDARG;
}

void STDMETHODCALLTYPE CommandList::ClearState(ID3D12PipelineState* state) {
    if (state != nullptr) {
        NotYet("ID3D12GraphicsCommandList::ClearState");
    }
}

Resource* CommandList::Ours(ID3D12Resource* resource, const char* function) const {
    Resource* ours = dynamic_cast<Resource*>(resource);
    if (ours == nullptr) {
        AbxUnsupported(function, "a resource that AnyBox did not create");
    }
    return ours;
}

CommandRecorder CommandList::Recorder(const char* function) const {
    if (closed_) {
        AbxUnsupported(function, "recording into a closed command list");
    }
    return CommandRecorder(context_.device, commands_);
}

void CommandList::Check(bool ok, const char* function, const std::string& error) const {
    if (!ok) {
        AbxUnsupported(function, error.c_str());
    }
}

D3D12_RESOURCE_STATES CommandList::StateOf(const Resource* resource, UINT subresource, D3D12_RESOURCE_STATES expected) const {
    auto found = states_.find({resource, subresource});
    if (found == states_.end()) {
        found = states_.find({resource, kAllSubresources});
    }
    return found != states_.end() ? found->second : expected;
}

BufferRef CommandList::WindowRange(D3D12_GPU_VIRTUAL_ADDRESS address, UINT64 bytes, const char* function) const {
    if (!context_.window.Contains(address, bytes)) {
        AbxUnsupported(function, "a buffer address outside the title buffer window");
    }
    return {context_.window.Buffer(), address - context_.window.Start(), bytes};
}

void STDMETHODCALLTYPE CommandList::DrawInstanced(UINT, UINT, UINT, UINT) { NotYet("ID3D12GraphicsCommandList::DrawInstanced"); }
void STDMETHODCALLTYPE CommandList::DrawIndexedInstanced(UINT, UINT, UINT, INT, UINT) { NotYet("ID3D12GraphicsCommandList::DrawIndexedInstanced"); }
void STDMETHODCALLTYPE CommandList::Dispatch(UINT, UINT, UINT) { NotYet("ID3D12GraphicsCommandList::Dispatch"); }

void STDMETHODCALLTYPE CommandList::CopyBufferRegion(ID3D12Resource* destination, UINT64 destinationOffset, ID3D12Resource* source, UINT64 sourceOffset, UINT64 bytes) {
    const char* function = "ID3D12GraphicsCommandList::CopyBufferRegion";
    Resource* to = Ours(destination, function);
    Resource* from = Ours(source, function);
    if (!to->IsBuffer() || !from->IsBuffer()) {
        AbxUnsupported(function, "a copy between buffers with a texture");
    }
    std::string error;
    Check(Recorder(function).CopyBufferRegion(to->Buffer(), destinationOffset, from->Buffer(), sourceOffset, bytes, error), function, error);
}

void STDMETHODCALLTYPE CommandList::CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* destination, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* source,
                                                     const D3D12_BOX* box) {
    const char* function = "ID3D12GraphicsCommandList::CopyTextureRegion";
    if (destination == nullptr || source == nullptr) {
        AbxUnsupported(function, "a missing copy location");
    }
    Resource* to = Ours(destination->pResource, function);
    Resource* from = Ours(source->pResource, function);
    std::string error;
    const bool toTexture = destination->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const bool fromTexture = source->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    if (toTexture != to->IsBuffer() && fromTexture != from->IsBuffer()) {
        if (toTexture && fromTexture) {
            const UINT toSub = destination->SubresourceIndex;
            const UINT fromSub = source->SubresourceIndex;
            Check(Recorder(function).CopyTexture(to->Texture(), StateOf(to, toSub, D3D12_RESOURCE_STATE_COPY_DEST), toSub, x, y, z, from->Texture(),
                                         StateOf(from, fromSub, D3D12_RESOURCE_STATE_COPY_SOURCE), fromSub, box, error),
                  function, error);
            return;
        }
        if (toTexture) {
            const UINT toSub = destination->SubresourceIndex;
            Check(Recorder(function).CopyBufferToTexture(to->Texture(), StateOf(to, toSub, D3D12_RESOURCE_STATE_COPY_DEST), toSub, x, y, z, from->Buffer(), source->PlacedFootprint, box,
                                                 error),
                  function, error);
            return;
        }
        if (fromTexture) {
            const UINT fromSub = source->SubresourceIndex;
            Check(Recorder(function).CopyTextureToFootprint(to->Buffer(), destination->PlacedFootprint, x, y, z, from->Texture(), StateOf(from, fromSub, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                                    fromSub, box, error),
                  function, error);
            return;
        }
    }
    AbxUnsupported(function, "copy locations whose types do not match their resources, or two footprints");
}

void STDMETHODCALLTYPE CommandList::CopyResource(ID3D12Resource* destination, ID3D12Resource* source) {
    const char* function = "ID3D12GraphicsCommandList::CopyResource";
    Resource* to = Ours(destination, function);
    Resource* from = Ours(source, function);
    std::string error;
    if (to->IsBuffer() && from->IsBuffer()) {
        if (to->Buffer().size != from->Buffer().size) {
            AbxUnsupported(function, "buffers of different sizes");
        }
        Check(Recorder(function).CopyBufferRegion(to->Buffer(), 0, from->Buffer(), 0, from->Buffer().size, error), function, error);
        return;
    }
    if (to->IsBuffer() || from->IsBuffer()) {
        AbxUnsupported(function, "a buffer and a texture");
    }
    const TextureRef& target = to->Texture();
    const TextureRef& origin = from->Texture();
    if (target.layout.info.mipLevels != origin.layout.info.mipLevels || target.layout.info.arrayLayers != origin.layout.info.arrayLayers ||
        target.desc.Width != origin.desc.Width || target.desc.Height != origin.desc.Height || target.desc.DepthOrArraySize != origin.desc.DepthOrArraySize) {
        AbxUnsupported(function, "textures of different shapes");
    }
    const UINT planes = (origin.layout.format.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0 && (origin.layout.format.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0 ? 2 : 1;
    const UINT layers = origin.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : origin.layout.info.arrayLayers;
    const UINT count = origin.layout.info.mipLevels * layers * planes;
    for (UINT subresource = 0; subresource < count; ++subresource) {
        Check(Recorder(function).CopyTexture(target, StateOf(to, subresource, D3D12_RESOURCE_STATE_COPY_DEST), subresource, 0, 0, 0, origin,
                                     StateOf(from, subresource, D3D12_RESOURCE_STATE_COPY_SOURCE), subresource, nullptr, error),
              function, error);
    }
}

void STDMETHODCALLTYPE CommandList::CopyTiles(ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*, ID3D12Resource*, UINT64, D3D12_TILE_COPY_FLAGS) {
    AbxUnsupported("ID3D12GraphicsCommandList::CopyTiles", "reserved resource tiles");
}

void STDMETHODCALLTYPE CommandList::ResolveSubresource(ID3D12Resource* destination, UINT destinationSubresource, ID3D12Resource* source, UINT sourceSubresource, DXGI_FORMAT format) {
    const char* function = "ID3D12GraphicsCommandList::ResolveSubresource";
    Resource* to = Ours(destination, function);
    Resource* from = Ours(source, function);
    if (to->IsBuffer() || from->IsBuffer()) {
        AbxUnsupported(function, "a buffer");
    }
    if (format != to->Texture().desc.Format && format != from->Texture().desc.Format) {
        AbxUnsupported(function, "a resolve format other than the textures' own");
    }
    std::string error;
    Check(Recorder(function).ResolveSubresource(to->Texture(), StateOf(to, destinationSubresource, D3D12_RESOURCE_STATE_RESOLVE_DEST), destinationSubresource, from->Texture(),
                                        StateOf(from, sourceSubresource, D3D12_RESOURCE_STATE_RESOLVE_SOURCE), sourceSubresource, error),
          function, error);
}

void STDMETHODCALLTYPE CommandList::IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology) {
    const char* function = "ID3D12GraphicsCommandList::IASetPrimitiveTopology";
    std::string error;
    Check(Recorder(function).SetPrimitiveTopology(topology, false, error), function, error);
}

void STDMETHODCALLTYPE CommandList::RSSetViewports(UINT count, const D3D12_VIEWPORT* viewports) {
    const char* function = "ID3D12GraphicsCommandList::RSSetViewports";
    if (count != 0 && viewports == nullptr) {
        AbxUnsupported(function, "viewports without a viewport array");
    }
    std::string error;
    const std::vector<D3D12_VIEWPORT> list = count != 0 ? std::vector<D3D12_VIEWPORT>(viewports, viewports + count) : std::vector<D3D12_VIEWPORT>{};
    Check(Recorder(function).SetViewports(list, error), function, error);
}

void STDMETHODCALLTYPE CommandList::RSSetScissorRects(UINT count, const D3D12_RECT* rects) {
    const char* function = "ID3D12GraphicsCommandList::RSSetScissorRects";
    if (count != 0 && rects == nullptr) {
        AbxUnsupported(function, "scissor rectangles without a rectangle array");
    }
    std::string error;
    const std::vector<D3D12_RECT> list = count != 0 ? std::vector<D3D12_RECT>(rects, rects + count) : std::vector<D3D12_RECT>{};
    Check(Recorder(function).SetScissors(list, error), function, error);
}

void STDMETHODCALLTYPE CommandList::OMSetBlendFactor(const FLOAT factor[4]) {
    const float ones[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    Recorder("ID3D12GraphicsCommandList::OMSetBlendFactor").SetBlendFactor(factor != nullptr ? factor : ones);
}

void STDMETHODCALLTYPE CommandList::OMSetStencilRef(UINT reference) {
    Recorder("ID3D12GraphicsCommandList::OMSetStencilRef").SetStencilReference(reference);
}

void STDMETHODCALLTYPE CommandList::SetPipelineState(ID3D12PipelineState*) { NotYet("ID3D12GraphicsCommandList::SetPipelineState"); }

void STDMETHODCALLTYPE CommandList::ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    const char* function = "ID3D12GraphicsCommandList::ResourceBarrier";
    if (count != 0 && barriers == nullptr) {
        AbxUnsupported(function, "barriers without a barrier array");
    }
    Recorder(function);
    std::vector<VkImageMemoryBarrier2> images;
    std::vector<VkMemoryBarrier2> memory;
    std::string error;
    for (UINT index = 0; index < count; ++index) {
        const D3D12_RESOURCE_BARRIER& barrier = barriers[index];
        if (barrier.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE) {
            AbxUnsupported(function, "split barriers (BEGIN_ONLY or END_ONLY)");
        }
        if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
            memory.push_back(UnorderedAccessBarrier());
            continue;
        }
        if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            memory.push_back(AliasingBarrier());
            continue;
        }
        if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            AbxUnsupported(function, "an unknown barrier type");
        }
        Resource* resource = Ours(barrier.Transition.pResource, function);
        const UINT subresource = barrier.Transition.Subresource;
        if (resource->IsBuffer()) {
            const std::optional<VkMemoryBarrier2> buffer = BufferTransition(barrier.Transition.StateBefore, barrier.Transition.StateAfter, error);
            Check(buffer.has_value(), function, error);
            memory.push_back(*buffer);
        } else {
            const TextureRef& texture = resource->Texture();
            const bool depth = (texture.layout.format.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
            std::optional<VkImageMemoryBarrier2> image = ImageTransition(barrier.Transition.StateBefore, barrier.Transition.StateAfter, depth, error);
            Check(image.has_value(), function, error);
            image->image = texture.image;
            if (subresource == kAllSubresources) {
                image->subresourceRange = {texture.layout.format.aspects, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            } else {
                const std::optional<Subresource> split = SplitSubresource(texture.desc, subresource, texture.layout.info.mipLevels);
                const std::optional<VkImageAspectFlags> aspect = split ? PlaneAspect(texture.layout.format.aspects, split->plane) : std::nullopt;
                if (!aspect) {
                    AbxUnsupported(function, "a barrier on a subresource outside the texture");
                }
                image->subresourceRange = {*aspect, split->mip, 1, texture.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 0u : split->layer, 1};
            }
            images.push_back(*image);
        }
        if (subresource == kAllSubresources) {
            for (auto entry = states_.begin(); entry != states_.end();) {
                entry = entry->first.first == resource ? states_.erase(entry) : std::next(entry);
            }
        }
        states_[{resource, subresource}] = barrier.Transition.StateAfter;
    }
    if (images.empty() && memory.empty()) {
        return;
    }
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = static_cast<std::uint32_t>(memory.size());
    dependency.pMemoryBarriers = memory.empty() ? nullptr : memory.data();
    dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(images.size());
    dependency.pImageMemoryBarriers = images.empty() ? nullptr : images.data();
    context_.device.Vk().cmdPipelineBarrier2(commands_, &dependency);
}

void STDMETHODCALLTYPE CommandList::ExecuteBundle(ID3D12GraphicsCommandList*) { AbxUnsupported("ID3D12GraphicsCommandList::ExecuteBundle", "bundles"); }
void STDMETHODCALLTYPE CommandList::SetDescriptorHeaps(UINT, ID3D12DescriptorHeap* const*) { NotYet("ID3D12GraphicsCommandList::SetDescriptorHeaps"); }
void STDMETHODCALLTYPE CommandList::SetComputeRootSignature(ID3D12RootSignature*) { NotYet("ID3D12GraphicsCommandList::SetComputeRootSignature"); }
void STDMETHODCALLTYPE CommandList::SetGraphicsRootSignature(ID3D12RootSignature*) { NotYet("ID3D12GraphicsCommandList::SetGraphicsRootSignature"); }
void STDMETHODCALLTYPE CommandList::SetComputeRootDescriptorTable(UINT, D3D12_GPU_DESCRIPTOR_HANDLE) { NotYet("ID3D12GraphicsCommandList::SetComputeRootDescriptorTable"); }
void STDMETHODCALLTYPE CommandList::SetGraphicsRootDescriptorTable(UINT, D3D12_GPU_DESCRIPTOR_HANDLE) { NotYet("ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable"); }
void STDMETHODCALLTYPE CommandList::SetComputeRoot32BitConstant(UINT, UINT, UINT) { NotYet("ID3D12GraphicsCommandList::SetComputeRoot32BitConstant"); }
void STDMETHODCALLTYPE CommandList::SetGraphicsRoot32BitConstant(UINT, UINT, UINT) { NotYet("ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstant"); }
void STDMETHODCALLTYPE CommandList::SetComputeRoot32BitConstants(UINT, UINT, const void*, UINT) { NotYet("ID3D12GraphicsCommandList::SetComputeRoot32BitConstants"); }
void STDMETHODCALLTYPE CommandList::SetGraphicsRoot32BitConstants(UINT, UINT, const void*, UINT) { NotYet("ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants"); }
void STDMETHODCALLTYPE CommandList::SetComputeRootConstantBufferView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) { NotYet("ID3D12GraphicsCommandList::SetComputeRootConstantBufferView"); }
void STDMETHODCALLTYPE CommandList::SetGraphicsRootConstantBufferView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) { NotYet("ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView"); }
void STDMETHODCALLTYPE CommandList::SetComputeRootShaderResourceView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) { NotYet("ID3D12GraphicsCommandList::SetComputeRootShaderResourceView"); }
void STDMETHODCALLTYPE CommandList::SetGraphicsRootShaderResourceView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) { NotYet("ID3D12GraphicsCommandList::SetGraphicsRootShaderResourceView"); }
void STDMETHODCALLTYPE CommandList::SetComputeRootUnorderedAccessView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) { NotYet("ID3D12GraphicsCommandList::SetComputeRootUnorderedAccessView"); }
void STDMETHODCALLTYPE CommandList::SetGraphicsRootUnorderedAccessView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) { NotYet("ID3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView"); }

void STDMETHODCALLTYPE CommandList::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view) {
    if (view == nullptr || view->BufferLocation == 0) {
        return;
    }
    const char* function = "ID3D12GraphicsCommandList::IASetIndexBuffer";
    std::string error;
    Check(Recorder(function).SetIndexBuffer(WindowRange(view->BufferLocation, view->SizeInBytes, function), view->Format, error), function, error);
}

void STDMETHODCALLTYPE CommandList::IASetVertexBuffers(UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW* views) {
    const char* function = "ID3D12GraphicsCommandList::IASetVertexBuffers";
    if (views == nullptr) {
        return;
    }
    std::string error;
    for (UINT index = 0; index < count; ++index) {
        if (views[index].BufferLocation == 0 || views[index].SizeInBytes == 0) {
            continue;
        }
        const BufferRef range = WindowRange(views[index].BufferLocation, views[index].SizeInBytes, function);
        Check(Recorder(function).SetVertexBuffers(start + index, {{range, views[index].StrideInBytes}}, error), function, error);
    }
}

void STDMETHODCALLTYPE CommandList::SOSetTargets(UINT, UINT, const D3D12_STREAM_OUTPUT_BUFFER_VIEW*) { AbxUnsupported("ID3D12GraphicsCommandList::SOSetTargets", "stream output"); }
void STDMETHODCALLTYPE CommandList::OMSetRenderTargets(UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*) {
    NotYet("ID3D12GraphicsCommandList::OMSetRenderTargets");
}
void STDMETHODCALLTYPE CommandList::ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*) {
    NotYet("ID3D12GraphicsCommandList::ClearDepthStencilView");
}
void STDMETHODCALLTYPE CommandList::ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT[4], UINT, const D3D12_RECT*) {
    NotYet("ID3D12GraphicsCommandList::ClearRenderTargetView");
}
void STDMETHODCALLTYPE CommandList::ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*, const UINT[4], UINT, const D3D12_RECT*) {
    NotYet("ID3D12GraphicsCommandList::ClearUnorderedAccessViewUint");
}
void STDMETHODCALLTYPE CommandList::ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*, const float[4], UINT, const D3D12_RECT*) {
    NotYet("ID3D12GraphicsCommandList::ClearUnorderedAccessViewFloat");
}
void STDMETHODCALLTYPE CommandList::BeginQuery(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT) { AbxUnsupported("ID3D12GraphicsCommandList::BeginQuery", "queries"); }
void STDMETHODCALLTYPE CommandList::EndQuery(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT) { AbxUnsupported("ID3D12GraphicsCommandList::EndQuery", "queries"); }
void STDMETHODCALLTYPE CommandList::ResolveQueryData(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT, UINT, ID3D12Resource*, UINT64) {
    AbxUnsupported("ID3D12GraphicsCommandList::ResolveQueryData", "queries");
}
void STDMETHODCALLTYPE CommandList::SetPredication(ID3D12Resource*, UINT64, D3D12_PREDICATION_OP) { AbxUnsupported("ID3D12GraphicsCommandList::SetPredication", "predication"); }
void STDMETHODCALLTYPE CommandList::ExecuteIndirect(ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64) {
    NotYet("ID3D12GraphicsCommandList::ExecuteIndirect");
}

}
