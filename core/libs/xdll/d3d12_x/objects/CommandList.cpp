#include "CommandList.hpp"

#include "AbxApi.hpp"
#include "BindlessSet.hpp"
#include "CopyLayout.hpp"
#include "ResourceState.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace abx::d3d12x {

namespace {

constexpr UINT kAllSubresources = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
constexpr std::uint32_t kGraphicsFirstRootSgpr = 8;
constexpr std::uint32_t kComputeFirstRootSgpr = 4;
constexpr std::uint32_t kUserSgprs = 32;
constexpr std::uint32_t kPageTableWord = 32;
constexpr std::uint32_t kPushWords = 36;
constexpr float kDefaultBlendFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};

[[noreturn]] void RootMappingUnknown(const char* function) {
    AbxUnsupported(function, "how root arguments, descriptor heaps and the heap base reach the translated shader's user data is unknown until a title's calls show it");
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
    if (initial != nullptr && dynamic_cast<PipelineStateObject*>(initial) == nullptr) {
        error = "an initial pipeline state that AnyBox did not create";
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
    copied_ = false;
    rendering_ = false;
    rectangles_ = false;
    graphicsSetBound_ = false;
    computeSetBound_ = false;
    ResetBindings();
    if (type_ == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        CommandRecorder recorder = Recorder("ID3D12GraphicsCommandList::Reset");
        recorder.SetBlendFactor(kDefaultBlendFactor);
        recorder.SetStencilReference(0);
    }
    if (initial != nullptr) BindPipeline(initial, "ID3D12GraphicsCommandList::Reset");
    return true;
}

void CommandList::EndPass() {
    if (!rendering_) return;
    Recorder("ID3D12GraphicsCommandList").EndRendering();
    rendering_ = false;
}

void CommandList::BindTitleSet(VkPipelineBindPoint point) {
    bool& bound = point == VK_PIPELINE_BIND_POINT_COMPUTE ? computeSetBound_ : graphicsSetBound_;
    if (bound) return;
    if (context_.bindless == nullptr) {
        AbxUnsupported("ID3D12GraphicsCommandList", "a draw or dispatch on a device without the bindless descriptor set");
    }
    Recorder("ID3D12GraphicsCommandList").BindTitleSet(point, context_.bindless->Set());
    bound = true;
}

void CommandList::PushUserData(const RootArguments& arguments, std::uint32_t firstSgpr, const char* function) {
    std::array<std::uint32_t, kPushWords> block{};
    const std::uint32_t words = arguments.signature != nullptr ? arguments.signature->Layout().words : 0;
    if (words > kUserSgprs - firstSgpr) {
        AbxUnsupported(function, "a root signature whose words do not fit in the user SGPRs after the driver's reserved ones; the console's spill layout is not measured");
    }
    std::copy_n(arguments.words.begin(), words, block.begin() + firstSgpr);
    if (context_.addresses == nullptr) {
        AbxUnsupported(function, "a draw or dispatch on a device without a title address space");
    }
    const std::uint64_t table = context_.addresses->RootTable();
    block[kPageTableWord] = static_cast<std::uint32_t>(table);
    block[kPageTableWord + 1] = static_cast<std::uint32_t>(table >> 32);
    std::string error;
    Check(Recorder(function).SetPushConstants(block.data(), static_cast<std::uint32_t>(block.size() * sizeof(std::uint32_t)), error), function, error);
}

void CommandList::PrepareDraw(const char* function) {
    if (pipeline_ == nullptr || pipeline_->BindPoint() != VK_PIPELINE_BIND_POINT_GRAPHICS) {
        AbxUnsupported(function, "a draw without a graphics pipeline state");
    }
    if (pipeline_->UsesCodeBase()) {
        AbxUnsupported(function, "a pipeline whose shaders read constants from their own code; placing shader code in title memory is not implemented");
    }
    if (pipeline_->HasStage("ms")) {
        AbxUnsupported(function, "a mesh pipeline: the mesh stage reads root words from s16 and the pixel stage from s8, which one push constant block cannot serve yet");
    }
    if (context_.device.RectangleFill()) {
        Recorder(function).SetPolygonMode(rectangles_ ? VK_POLYGON_MODE_FILL_RECTANGLE_NV : pipeline_->PolygonMode());
    } else if (rectangles_) {
        AbxUnsupported(function, "a rectangle list on a device without VK_NV_fill_rectangle and dynamic polygon mode");
    }
    PushUserData(graphics_, kGraphicsFirstRootSgpr, function);
    BindTitleSet(VK_PIPELINE_BIND_POINT_GRAPHICS);
    if (rendering_) return;
    VkExtent2D extent{UINT32_MAX, UINT32_MAX};
    std::uint32_t layers = UINT32_MAX;
    bool any = false;
    const auto include = [&](const AttachmentRef& attachment) {
        if (attachment.view == VK_NULL_HANDLE) return;
        extent.width = (std::min)(extent.width, attachment.extent.width);
        extent.height = (std::min)(extent.height, attachment.extent.height);
        layers = (std::min)(layers, attachment.layers);
        any = true;
    };
    for (const AttachmentRef& color : renderTargets_) include(color);
    if (depthStencil_) include(*depthStencil_);
    if (!any) {
        AbxUnsupported(function, "a draw with no render target and no depth-stencil view bound");
    }
    std::string error;
    Check(Recorder(function).BeginRendering(renderTargets_, depthStencil_ ? &*depthStencil_ : nullptr, extent, layers, error), function, error);
    rendering_ = true;
}

void CommandList::OrderCopy(const char* function) {
    EndPass();
    Recorder(function);
    if (copied_) {
        VkMemoryBarrier2 order{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        order.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        order.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        order.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        order.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &order;
        context_.device.Vk().cmdPipelineBarrier2(commands_, &dependency);
    }
    copied_ = true;
}

void CommandList::ResetBindings() {
    shaderResourceHeap_ = nullptr;
    samplerHeap_ = nullptr;
    renderTargets_.clear();
    depthStencil_.reset();
    pipeline_ = nullptr;
    graphics_ = RootArguments{};
    compute_ = RootArguments{};
}

void CommandList::BindPipeline(ID3D12PipelineState* state, const char* function) {
    PipelineStateObject* ours = dynamic_cast<PipelineStateObject*>(state);
    if (ours == nullptr) {
        AbxUnsupported(function, "a pipeline state that AnyBox did not create");
    }
    if (ours->Pipeline() == VK_NULL_HANDLE) {
        AbxUnsupported(function, ("a pipeline state that has no Vulkan pipeline: " + ours->UnbuiltReason()).c_str());
    }
    if (ours->BindPoint() == VK_PIPELINE_BIND_POINT_GRAPHICS && type_ != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        AbxUnsupported(function, "a graphics pipeline state on a compute or copy command list");
    }
    Recorder(function).BindPipeline(ours->BindPoint(), ours->Pipeline());
    pipeline_ = ours;
}

void CommandList::SetSignature(RootArguments& arguments, ID3D12RootSignature* signature, const char* function) {
    Recorder(function);
    const RootSignature* ours = nullptr;
    if (signature != nullptr) {
        ours = dynamic_cast<RootSignature*>(signature);
        if (ours == nullptr) {
            AbxUnsupported(function, "a root signature that AnyBox did not create");
        }
    }
    if (ours != arguments.signature) {
        arguments = RootArguments{};
        arguments.signature = ours;
    }
}

const RootParameterLayout& CommandList::Parameter(const RootArguments& arguments, UINT index, D3D12_ROOT_PARAMETER_TYPE type, const char* function) const {
    Recorder(function);
    if (arguments.signature == nullptr) {
        AbxUnsupported(function, "a root argument before a root signature is set");
    }
    const std::vector<RootParameterLayout>& parameters = arguments.signature->Layout().parameters;
    if (index >= parameters.size() || parameters[index].type != type) {
        AbxUnsupported(function, "a root parameter index outside the root signature, or of another type");
    }
    return parameters[index];
}

void CommandList::SetConstants(RootArguments& arguments, UINT index, UINT count, const void* data, UINT offset, const char* function) {
    const RootParameterLayout& parameter = Parameter(arguments, index, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, function);
    if (offset > parameter.constants || count > parameter.constants - offset || (count != 0 && data == nullptr)) {
        AbxUnsupported(function, "root constants outside their parameter, or without data");
    }
    if (count != 0) std::memcpy(arguments.words.data() + parameter.firstWord + offset, data, static_cast<std::size_t>(count) * sizeof(std::uint32_t));
}

void CommandList::SetRootView(RootArguments& arguments, UINT index, D3D12_ROOT_PARAMETER_TYPE type, D3D12_GPU_VIRTUAL_ADDRESS address, const char* function) {
    const RootParameterLayout& parameter = Parameter(arguments, index, type, function);
    if (address != 0 && !context_.window.Contains(address, 1) && (context_.addresses == nullptr || !context_.addresses->Contains(address, 1))) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            AbxLog("d3d12_x", "%s: root view address 0x%llx is in no GPU-visible title memory; shader reads there return zero (later cases are not logged)", function,
                   static_cast<unsigned long long>(address));
        }
    }
    arguments.words[parameter.firstWord] = static_cast<std::uint32_t>(address);
    arguments.words[parameter.firstWord + 1] = static_cast<std::uint32_t>(address >> 32);
}

void CommandList::SetTable(RootArguments& arguments, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle, const char* function) {
    const RootParameterLayout& parameter = Parameter(arguments, index, D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, function);
    const bool samplers = !parameter.ranges.empty() && parameter.ranges.front().RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    const DescriptorHeap* heap = samplers ? samplerHeap_ : shaderResourceHeap_;
    if (heap == nullptr) {
        AbxUnsupported(function, "a descriptor table before SetDescriptorHeaps bound a heap of its type");
    }
    const std::uint64_t start = heap->GpuStart().ptr;
    const std::uint64_t end = start + static_cast<std::uint64_t>(heap->Count()) * heap->Increment();
    if (handle.ptr < start || handle.ptr >= end || (handle.ptr - start) % heap->Increment() != 0) {
        AbxUnsupported(function, "a descriptor table handle outside the bound heap of its type");
    }
    if (handle.ptr > 0xFFFFFFFFull) {
        AbxUnsupported(function, "a descriptor table above 4 GB, which a single root word cannot hold");
    }
    arguments.words[parameter.firstWord] = static_cast<std::uint32_t>(handle.ptr);
}

const DescriptorRecord& CommandList::Attachment(D3D12_CPU_DESCRIPTOR_HANDLE handle, D3D12_DESCRIPTOR_HEAP_TYPE type, const char* function) const {
    UINT index = 0;
    const DescriptorHeap* heap = DescriptorHeap::Find(handle.ptr, index);
    if (heap == nullptr || heap->Type() != type) {
        AbxUnsupported(function, "a descriptor handle outside every render target or depth-stencil heap AnyBox created");
    }
    return heap->Record(index);
}

HRESULT STDMETHODCALLTYPE CommandList::Close() {
    if (closed_) {
        return E_FAIL;
    }
    EndPass();
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
    ResetBindings();
    if (state != nullptr) BindPipeline(state, "ID3D12GraphicsCommandList::ClearState");
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
    if (context_.window.Contains(address, bytes)) return {context_.window.Buffer(), address - context_.window.Start(), bytes};
    if (context_.addresses != nullptr) {
        if (const std::optional<TitleAddressSpace::Location> found = context_.addresses->Locate(address, bytes)) return {found->buffer, found->offset, bytes};
    }
    char reason[128];
    std::snprintf(reason, sizeof(reason), "a buffer range 0x%llx (+%llu bytes) that is neither in the buffer window nor in imported title memory",
                  static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes));
    AbxUnsupported(function, reason);
}

void CommandList::ReportRootState(const RootArguments& arguments, const char* function) const {
    std::string text = "pipeline:";
    if (pipeline_ != nullptr) {
        for (const std::string& shader : pipeline_->Shaders()) text += " " + shader;
    }
    if (arguments.signature != nullptr) {
        const RootSignatureLayout& layout = arguments.signature->Layout();
        text += "; root signature " + std::to_string(layout.parameters.size()) + " parameters, " + std::to_string(layout.words) + " words:";
        for (std::size_t index = 0; index < layout.parameters.size(); ++index) {
            const RootParameterLayout& parameter = layout.parameters[index];
            char line[96];
            std::snprintf(line, sizeof(line), " [%zu type %d visibility %d register %u space %u words %u at %u]", index, static_cast<int>(parameter.type),
                          static_cast<int>(parameter.visibility), parameter.shaderRegister, parameter.registerSpace, parameter.words, parameter.firstWord);
            text += line;
        }
        text += "; words:";
        for (std::uint32_t word = 0; word < layout.words; ++word) {
            char value[16];
            std::snprintf(value, sizeof(value), " %08x", arguments.words[word]);
            text += value;
        }
    } else {
        text += "; no root signature";
    }
    AbxLog("d3d12_x", "%s root state: %s", function, text.c_str());
}

void STDMETHODCALLTYPE CommandList::DrawInstanced(UINT vertices, UINT instances, UINT firstVertex, UINT firstInstance) {
    const char* function = "ID3D12GraphicsCommandList::DrawInstanced";
    PrepareDraw(function);
    Recorder(function).Draw(vertices, instances, firstVertex, firstInstance);
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) ReportRootState(graphics_, "first DrawInstanced");
}

void STDMETHODCALLTYPE CommandList::DrawIndexedInstanced(UINT indices, UINT instances, UINT firstIndex, INT baseVertex, UINT firstInstance) {
    const char* function = "ID3D12GraphicsCommandList::DrawIndexedInstanced";
    PrepareDraw(function);
    Recorder(function).DrawIndexed(indices, instances, firstIndex, baseVertex, firstInstance);
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) ReportRootState(graphics_, "first DrawIndexedInstanced");
}

void STDMETHODCALLTYPE CommandList::Dispatch(UINT x, UINT y, UINT z) {
    const char* function = "ID3D12GraphicsCommandList::Dispatch";
    EndPass();
    if (pipeline_ == nullptr || pipeline_->BindPoint() != VK_PIPELINE_BIND_POINT_COMPUTE) {
        AbxUnsupported(function, "a dispatch without a compute pipeline state");
    }
    if (pipeline_->UsesCodeBase()) {
        AbxUnsupported(function, "a compute shader that reads constants from its own code; placing shader code in title memory is not implemented");
    }
    PushUserData(compute_, kComputeFirstRootSgpr, function);
    BindTitleSet(VK_PIPELINE_BIND_POINT_COMPUTE);
    Recorder(function).Dispatch(x, y, z);
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) ReportRootState(compute_, "first Dispatch");
}

void STDMETHODCALLTYPE CommandList::CopyBufferRegion(ID3D12Resource* destination, UINT64 destinationOffset, ID3D12Resource* source, UINT64 sourceOffset, UINT64 bytes) {
    const char* function = "ID3D12GraphicsCommandList::CopyBufferRegion";
    Resource* to = Ours(destination, function);
    Resource* from = Ours(source, function);
    if (!to->IsBuffer() || !from->IsBuffer()) {
        AbxUnsupported(function, "a copy between buffers with a texture");
    }
    std::string error;
    OrderCopy(function);
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
    OrderCopy(function);
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
    OrderCopy(function);
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
    EndPass();
    Resource* to = Ours(destination, function);
    Resource* from = Ours(source, function);
    if (to->IsBuffer() || from->IsBuffer()) {
        AbxUnsupported(function, "a buffer");
    }
    if (format != to->Texture().desc.Format && format != from->Texture().desc.Format) {
        AbxUnsupported(function, "a resolve format other than the textures' own");
    }
    std::string error;
    OrderCopy(function);
    Check(Recorder(function).ResolveSubresource(to->Texture(), StateOf(to, destinationSubresource, D3D12_RESOURCE_STATE_RESOLVE_DEST), destinationSubresource, from->Texture(),
                                        StateOf(from, sourceSubresource, D3D12_RESOURCE_STATE_RESOLVE_SOURCE), sourceSubresource, error),
          function, error);
}

void STDMETHODCALLTYPE CommandList::IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology) {
    const char* function = "ID3D12GraphicsCommandList::IASetPrimitiveTopology";
    std::string error;
    Check(Recorder(function).SetPrimitiveTopology(topology, false, error), function, error);
    rectangles_ = topology == kConsoleRectangleList;
}

void STDMETHODCALLTYPE CommandList::RSSetViewports(UINT count, const D3D12_VIEWPORT* viewports) {
    const char* function = "ID3D12GraphicsCommandList::RSSetViewports";
    if (count != 0 && viewports == nullptr) {
        AbxUnsupported(function, "viewports without a viewport array");
    }
    if (count == 0) {
        Recorder(function);
        return;
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
    if (count == 0) {
        Recorder(function);
        return;
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

void STDMETHODCALLTYPE CommandList::SetPipelineState(ID3D12PipelineState* state) {
    BindPipeline(state, "ID3D12GraphicsCommandList::SetPipelineState");
}

void STDMETHODCALLTYPE CommandList::ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    const char* function = "ID3D12GraphicsCommandList::ResourceBarrier";
    EndPass();
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
    copied_ = false;
}

void STDMETHODCALLTYPE CommandList::ExecuteBundle(ID3D12GraphicsCommandList*) { AbxUnsupported("ID3D12GraphicsCommandList::ExecuteBundle", "bundles"); }
void STDMETHODCALLTYPE CommandList::SetDescriptorHeaps(UINT count, ID3D12DescriptorHeap* const* heaps) {
    const char* function = "ID3D12GraphicsCommandList::SetDescriptorHeaps";
    Recorder(function);
    if (count != 0 && heaps == nullptr) {
        AbxUnsupported(function, "descriptor heaps without a heap array");
    }
    DescriptorHeap* shaderResource = nullptr;
    DescriptorHeap* sampler = nullptr;
    for (UINT index = 0; index < count; ++index) {
        DescriptorHeap* heap = dynamic_cast<DescriptorHeap*>(heaps[index]);
        if (heap == nullptr) {
            AbxUnsupported(function, "a descriptor heap that AnyBox did not create");
        }
        if (!heap->ShaderVisible()) {
            AbxUnsupported(function, "a descriptor heap that is not shader visible");
        }
        DescriptorHeap*& slot = heap->Type() == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? sampler : shaderResource;
        if (slot != nullptr) {
            AbxUnsupported(function, "two descriptor heaps of the same type");
        }
        slot = heap;
    }
    shaderResourceHeap_ = shaderResource;
    samplerHeap_ = sampler;
}
void STDMETHODCALLTYPE CommandList::SetComputeRootSignature(ID3D12RootSignature* signature) {
    SetSignature(compute_, signature, "ID3D12GraphicsCommandList::SetComputeRootSignature");
}
void STDMETHODCALLTYPE CommandList::SetGraphicsRootSignature(ID3D12RootSignature* signature) {
    SetSignature(graphics_, signature, "ID3D12GraphicsCommandList::SetGraphicsRootSignature");
}
void STDMETHODCALLTYPE CommandList::SetComputeRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    SetTable(compute_, index, handle, "ID3D12GraphicsCommandList::SetComputeRootDescriptorTable");
}
void STDMETHODCALLTYPE CommandList::SetGraphicsRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    SetTable(graphics_, index, handle, "ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable");
}
void STDMETHODCALLTYPE CommandList::SetComputeRoot32BitConstant(UINT index, UINT value, UINT offset) {
    SetConstants(compute_, index, 1, &value, offset, "ID3D12GraphicsCommandList::SetComputeRoot32BitConstant");
}
void STDMETHODCALLTYPE CommandList::SetGraphicsRoot32BitConstant(UINT index, UINT value, UINT offset) {
    SetConstants(graphics_, index, 1, &value, offset, "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstant");
}
void STDMETHODCALLTYPE CommandList::SetComputeRoot32BitConstants(UINT index, UINT count, const void* data, UINT offset) {
    SetConstants(compute_, index, count, data, offset, "ID3D12GraphicsCommandList::SetComputeRoot32BitConstants");
}
void STDMETHODCALLTYPE CommandList::SetGraphicsRoot32BitConstants(UINT index, UINT count, const void* data, UINT offset) {
    SetConstants(graphics_, index, count, data, offset, "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants");
}
void STDMETHODCALLTYPE CommandList::SetComputeRootConstantBufferView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
    SetRootView(compute_, index, D3D12_ROOT_PARAMETER_TYPE_CBV, address, "ID3D12GraphicsCommandList::SetComputeRootConstantBufferView");
}
void STDMETHODCALLTYPE CommandList::SetGraphicsRootConstantBufferView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
    SetRootView(graphics_, index, D3D12_ROOT_PARAMETER_TYPE_CBV, address, "ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView");
}
void STDMETHODCALLTYPE CommandList::SetComputeRootShaderResourceView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
    SetRootView(compute_, index, D3D12_ROOT_PARAMETER_TYPE_SRV, address, "ID3D12GraphicsCommandList::SetComputeRootShaderResourceView");
}
void STDMETHODCALLTYPE CommandList::SetGraphicsRootShaderResourceView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
    SetRootView(graphics_, index, D3D12_ROOT_PARAMETER_TYPE_SRV, address, "ID3D12GraphicsCommandList::SetGraphicsRootShaderResourceView");
}
void STDMETHODCALLTYPE CommandList::SetComputeRootUnorderedAccessView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
    SetRootView(compute_, index, D3D12_ROOT_PARAMETER_TYPE_UAV, address, "ID3D12GraphicsCommandList::SetComputeRootUnorderedAccessView");
}
void STDMETHODCALLTYPE CommandList::SetGraphicsRootUnorderedAccessView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
    SetRootView(graphics_, index, D3D12_ROOT_PARAMETER_TYPE_UAV, address, "ID3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView");
}

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
void STDMETHODCALLTYPE CommandList::OMSetRenderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* targets, BOOL singleRange, const D3D12_CPU_DESCRIPTOR_HANDLE* depthStencil) {
    const char* function = "ID3D12GraphicsCommandList::OMSetRenderTargets";
    EndPass();
    Recorder(function);
    if (count > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT || (count != 0 && targets == nullptr)) {
        AbxUnsupported(function, "more than eight render targets, or render targets without a handle array");
    }
    std::vector<AttachmentRef> colors;
    colors.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = targets[singleRange ? 0 : index];
        if (singleRange) {
            UINT first = 0;
            const DescriptorHeap* heap = DescriptorHeap::Find(handle.ptr, first);
            if (heap == nullptr) {
                AbxUnsupported(function, "a descriptor handle outside every render target heap AnyBox created");
            }
            handle.ptr += static_cast<SIZE_T>(index) * heap->Increment();
        }
        const DescriptorRecord& record = Attachment(handle, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, function);
        colors.push_back(record.kind == HeapEntry::RenderTarget ? record.attachment : AttachmentRef{});
    }
    std::optional<AttachmentRef> depth;
    if (depthStencil != nullptr) {
        const DescriptorRecord& record = Attachment(*depthStencil, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, function);
        if (record.kind == HeapEntry::DepthStencil) depth = record.attachment;
    }
    renderTargets_ = std::move(colors);
    depthStencil_ = depth;
}

void STDMETHODCALLTYPE CommandList::ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE view, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil, UINT count, const D3D12_RECT* rects) {
    const char* function = "ID3D12GraphicsCommandList::ClearDepthStencilView";
    EndPass();
    if (count != 0 && rects == nullptr) {
        AbxUnsupported(function, "clear rectangles without a rectangle array");
    }
    const DescriptorRecord& record = Attachment(view, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, function);
    if (record.kind != HeapEntry::DepthStencil) {
        AbxUnsupported(function, "clearing a null depth-stencil view");
    }
    std::string error;
    const std::vector<D3D12_RECT> list = count != 0 ? std::vector<D3D12_RECT>(rects, rects + count) : std::vector<D3D12_RECT>{};
    Check(Recorder(function).ClearDepthStencil(record.attachment, flags, depth, stencil, list, error), function, error);
}

void STDMETHODCALLTYPE CommandList::ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE view, const FLOAT color[4], UINT count, const D3D12_RECT* rects) {
    const char* function = "ID3D12GraphicsCommandList::ClearRenderTargetView";
    EndPass();
    if (color == nullptr || (count != 0 && rects == nullptr)) {
        AbxUnsupported(function, "a clear without a color, or clear rectangles without a rectangle array");
    }
    const DescriptorRecord& record = Attachment(view, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, function);
    if (record.kind != HeapEntry::RenderTarget) {
        AbxUnsupported(function, "clearing a null render target view");
    }
    std::string error;
    const std::vector<D3D12_RECT> list = count != 0 ? std::vector<D3D12_RECT>(rects, rects + count) : std::vector<D3D12_RECT>{};
    Check(Recorder(function).ClearRenderTarget(record.attachment, color, list, error), function, error);
}
void STDMETHODCALLTYPE CommandList::ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*, const UINT[4], UINT, const D3D12_RECT*) {
    AbxUnsupported("ID3D12GraphicsCommandList::ClearUnorderedAccessViewUint", "unordered access view clears are not implemented yet");
}
void STDMETHODCALLTYPE CommandList::ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*, const float[4], UINT, const D3D12_RECT*) {
    AbxUnsupported("ID3D12GraphicsCommandList::ClearUnorderedAccessViewFloat", "unordered access view clears are not implemented yet");
}
void STDMETHODCALLTYPE CommandList::BeginQuery(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT) { AbxUnsupported("ID3D12GraphicsCommandList::BeginQuery", "queries"); }
void STDMETHODCALLTYPE CommandList::EndQuery(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT) { AbxUnsupported("ID3D12GraphicsCommandList::EndQuery", "queries"); }
void STDMETHODCALLTYPE CommandList::ResolveQueryData(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT, UINT, ID3D12Resource*, UINT64) {
    AbxUnsupported("ID3D12GraphicsCommandList::ResolveQueryData", "queries");
}
void STDMETHODCALLTYPE CommandList::SetPredication(ID3D12Resource*, UINT64, D3D12_PREDICATION_OP) { AbxUnsupported("ID3D12GraphicsCommandList::SetPredication", "predication"); }
void STDMETHODCALLTYPE CommandList::ExecuteIndirect(ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64) {
    RootMappingUnknown("ID3D12GraphicsCommandList::ExecuteIndirect");
}

}
