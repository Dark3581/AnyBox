#include "CommandList.hpp"
#include "CommandSignature.hpp"
#include "FrameDump.hpp"
#include "IndirectClamp.hpp"
#include "DescriptorWriter.hpp"
#include "DescriptorHeap.hpp"
#include "LiveStats.hpp"
#include "QueryHeap.hpp"

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
#include <mutex>
#include <set>
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
    allocator_ = pool;
    closed_ = false;
    targetsSet_ = false;
    emptyTargets_ = 0;
    depthHandleGiven_ = false;
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

void CommandList::PushUserData(RootArguments& arguments, std::uint32_t firstSgpr, const char* function) {
    if (!arguments.pending.empty() && pipeline_ != nullptr && pipeline_->Signature() != nullptr && pipeline_->Signature() != arguments.signature) {
        arguments.signature = pipeline_->Signature();
        ApplyPending(arguments);
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            AbxLog("d3d12_x", "work with root constants that fit no bound root signature uses its pipeline's own root signature, keeping the argument words, as the console's registers would (first by %s)",
                   function);
        }
    }
    if (!arguments.pending.empty()) {
        const PendingConstants& first = arguments.pending.front();
        AbxUnsupported(function, ("work with root constants that fit no bound root signature (parameter " + std::to_string(first.index) + ", " +
                                  std::to_string(first.values.size()) + " values at offset " + std::to_string(first.offset) + ")")
                                     .c_str());
    }
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

namespace {

constexpr std::uint64_t kNoFrame = ~std::uint64_t{0};
std::atomic<std::uint64_t> g_computeSkipFrame{kNoFrame};

void NoteComputeSkip(const PipelineStateObject& pipeline, const char* reason) {
    g_computeSkipFrame.store(Stats().presents.load());
    static std::mutex lock;
    static std::set<std::string> seen;
    const std::string shader = pipeline.Shaders().empty() ? std::string("(no shader)") : pipeline.Shaders().front();
    const std::lock_guard guard(lock);
    if (seen.size() >= 160 || !seen.insert(shader + reason).second) return;
    AbxLog("d3d12_x", "DIAG first skip of compute shader %s: %s", shader.substr(0, 22).c_str(), reason);
}

bool IndirectDispatchGuarded() {
    static const bool enabled = [] {
        const char* value = AbxSetting("d3d12_x.indirect_dispatch_guard");
        return value != nullptr && value[0] == '1';
    }();
    const std::uint64_t last = g_computeSkipFrame.load();
    return enabled && last != kNoFrame && Stats().presents.load() <= last + 1;
}

}

bool CommandList::SkippedWork(const char* function) {
    enum Reason { Pending, Unbuilt, CodeBase, Count };
    static const char* const kReasons[Count] = {
        "its pipeline is still being built in the background",
        "its pipeline has no Vulkan pipeline",
        "its shaders read constants from their own code, and placing shader code in title memory is not implemented",
    };
    static std::atomic<std::uint64_t> skipped[Count]{};
    Reason reason = Count;
    if (pipeline_->Pending() && pipeline_->BindPoint() == VK_PIPELINE_BIND_POINT_COMPUTE) {
        static std::atomic<std::uint64_t> waited{0};
        const std::uint64_t count = ++waited;
        if (count == 1 || (count & (count - 1)) == 0) {
            AbxLog("d3d12_x", "compute work whose pipeline is still being built waits for that build instead of being skipped, as titles can depend on its results (%llu so far)",
                   static_cast<unsigned long long>(count));
        }
        pipeline_->BuildNow();
    }
    if (pipeline_->Pending()) {
        reason = Pending;
        pipeline_->Prioritize();
    }
    else if (pipeline_->Pipeline() == VK_NULL_HANDLE) reason = Unbuilt;
    else if (pipeline_->UsesCodeBase()) reason = CodeBase;
    static const int diagSkip = [] {
        char value[16] = {};
        GetEnvironmentVariableA("ANYBOX_DIAG_SKIP", value, sizeof(value));
        return value[0] == 'c' ? 1 : value[0] == 'g' ? 2 : 0;
    }();
    if (reason == Count && ((diagSkip == 1 && pipeline_->BindPoint() == VK_PIPELINE_BIND_POINT_COMPUTE) || (diagSkip == 2 && pipeline_->BindPoint() == VK_PIPELINE_BIND_POINT_GRAPHICS))) return true;
    if (reason == Count && context_.device.Checkpoints()) {
        static std::mutex markerLock;
        static std::set<std::string> markers;
        std::string name;
        for (const std::string& shader : pipeline_->Shaders()) name += (name.empty() ? "" : " ") + shader.substr(0, 20);
        const std::lock_guard guard(markerLock);
        Recorder(function).Checkpoint(markers.insert(std::string(function) + " " + name).first->c_str());
    }
    if (TracingFrame()) {
        for (Resource* target : targetResources_) NoteTracedTarget(target);
        std::string targets;
        for (const Resource* target : targetResources_) {
            char text[96];
            if (target == nullptr || target->IsBuffer()) {
                std::snprintf(text, sizeof(text), " (none)");
            } else {
                const D3D12_RESOURCE_DESC& desc = target->Texture().desc;
                std::snprintf(text, sizeof(text), " %p %llux%u f%d", static_cast<void*>(target->Texture().image), static_cast<unsigned long long>(desc.Width), desc.Height,
                              static_cast<int>(desc.Format));
            }
            targets += text;
        }
        std::string shaders;
        for (const std::string& shader : pipeline_->Shaders()) shaders += " " + shader.substr(0, 19);
        AbxLog("d3d12_x", "TRACE %s%s%s | targets%s%s%s", function, shaders.c_str(), reason == Count ? "" : (" SKIPPED: " + pipeline_->UnbuiltReason()).c_str(),
               targets.c_str(), depthStencil_ ? " +depth" : "", rectangles_ ? " rects" : "");
        const RootArguments& arguments = pipeline_->BindPoint() == VK_PIPELINE_BIND_POINT_COMPUTE ? compute_ : graphics_;
        {
            UINT increment = 0;
            const std::uint8_t* table = DescriptorHeap::AtGpuAddress(arguments.words[0], increment);
            if (table != nullptr) {
                std::uint32_t slots[16] = {};
                std::memcpy(slots, table, sizeof(slots));
                AbxLog("d3d12_x", "TRACE     table 0x%x at record: %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x", arguments.words[0], slots[0], slots[1],
                       slots[2], slots[3], slots[4], slots[5], slots[6], slots[7], slots[8], slots[9], slots[10], slots[11], slots[12], slots[13], slots[14], slots[15]);
                NoteTracedTable(arguments.words[0]);
                const std::uint64_t base = (static_cast<std::uint64_t>(slots[0]) | (static_cast<std::uint64_t>(slots[1] & 0xFFFF) << 32));
                if (context_.addresses != nullptr && base != 0) {
                    AbxLog("d3d12_x", "TRACE     V# base 0x%llx (%u bytes) is %s the GPU page table: %s", static_cast<unsigned long long>(base), slots[2],
                           context_.addresses->Contains(base, slots[2]) ? "in" : "NOT in", context_.addresses->DescribeTitleAddress(base).c_str());
                }
            }
        }
        for (std::size_t word = 0; word < arguments.words.size(); ++word) {
            UINT increment = 0;
            const std::uint8_t* table = DescriptorHeap::AtGpuAddress(arguments.words[word], increment);
            if (table == nullptr || increment < 32) continue;
            for (UINT slot = 0; slot < 6; ++slot) {
                std::uint32_t descriptor[8] = {};
                std::memcpy(descriptor, table + static_cast<std::size_t>(slot) * increment, sizeof(descriptor));
                if ((descriptor[3] >> 28) < 8) continue;
                AbxLog("d3d12_x", "TRACE     root word %zu slot %u: %s", word, slot, DescriptorWriter::DescribeImageWords(descriptor).c_str());
            }
        }
    }
    if (reason == Count) {
        ++(pipeline_->BindPoint() == VK_PIPELINE_BIND_POINT_COMPUTE ? Stats().dispatches : Stats().draws);
        if (!pipelineBound_) {
            Recorder(function).BindPipeline(pipeline_->BindPoint(), pipeline_->Pipeline());
            pipelineBound_ = true;
        }
        return false;
    }
    if (pipeline_->BindPoint() == VK_PIPELINE_BIND_POINT_COMPUTE) NoteComputeSkip(*pipeline_, reason == Pending ? kReasons[Pending] : pipeline_->UnbuiltReason().empty() ? kReasons[reason] : pipeline_->UnbuiltReason().c_str());
    ++(reason == Pending ? Stats().skippedBuilding : Stats().skippedOther);
    const std::uint64_t total = ++skipped[reason];
    bool power = total == 1;
    for (std::uint64_t step = 10; step <= total && !power; step *= 10) power = total == step;
    if (power) {
        AbxLog("d3d12_x", "%s skipped (%llu so far for this reason): %s%s%s", function, static_cast<unsigned long long>(total), kReasons[reason],
               reason == Unbuilt ? ": " : "", reason == Unbuilt ? pipeline_->UnbuiltReason().c_str() : "");
    }
    return true;
}

bool CommandList::PrepareDraw(const char* function) {
    if (pipeline_ == nullptr || pipeline_->BindPoint() != VK_PIPELINE_BIND_POINT_GRAPHICS) {
        AbxUnsupported(function, pipeline_ == nullptr ? "a draw with no pipeline state set on this command list since its last reset"
                                                      : ("a draw while a compute pipeline state is set: " + std::string(pipeline_->Shaders().empty() ? pipeline_->UnbuiltReason() : pipeline_->Shaders().front())).c_str());
    }
    if (SkippedWork(function)) return false;
    if (context_.device.RectangleFill()) {
        Recorder(function).SetPolygonMode(rectangles_ ? VK_POLYGON_MODE_FILL_RECTANGLE_NV : pipeline_->PolygonMode());
    } else if (rectangles_) {
        AbxUnsupported(function, "a rectangle list on a device without VK_NV_fill_rectangle and dynamic polygon mode");
    }
    PushUserData(graphics_, kGraphicsFirstRootSgpr, function);
    BindTitleSet(VK_PIPELINE_BIND_POINT_GRAPHICS);
    if (rendering_) return true;
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
    std::vector<AttachmentRef> padded;
    if (!any) {
        const VkPhysicalDeviceLimits& limits = context_.device.Properties().limits;
        extent = {limits.maxFramebufferWidth, limits.maxFramebufferHeight};
        layers = 1;
        UINT declared = 0;
        std::string formats = "unknown (no description kept)";
        if (const GraphicsDescription* description = pipeline_->Description()) {
            declared = (std::min)(description->desc.NumRenderTargets, UINT{8});
            formats = std::to_string(declared) + " render target(s)";
            for (UINT index = 0; index < declared; ++index) formats += " f" + std::to_string(static_cast<int>(description->desc.RTVFormats[index]));
            formats += ", depth f" + std::to_string(static_cast<int>(description->desc.DSVFormat));
        }
        if (declared > renderTargets_.size()) {
            padded = renderTargets_;
            padded.resize(declared);
        }
        static std::atomic<int> reported{0};
        if (reported.fetch_add(1) < 12) {
            std::string shaders;
            for (const std::string& shader : pipeline_->Shaders()) shaders += " " + shader.substr(0, 19);
            const std::string bound = !targetsSet_ ? std::string("OMSetRenderTargets was not called on this list since its reset")
                                                   : "OMSetRenderTargets gave " + std::to_string(renderTargets_.size()) + " target(s), " + std::to_string(emptyTargets_) +
                                                         " of them empty descriptors, " + (depthHandleGiven_ ? "and a depth-stencil handle that is an empty descriptor" : "and no depth-stencil handle");
            AbxLog("d3d12_x", "DIAG %s with nothing bound to render to: %s; the pipeline declares %s; shaders%s; drawn with no attachments (only its UAV writes have an effect)", function,
                   bound.c_str(), formats.c_str(), shaders.c_str());
        }
    }
    std::string error;
    Check(Recorder(function).BeginRendering(padded.empty() ? renderTargets_ : padded, depthStencil_ ? &*depthStencil_ : nullptr, extent, layers, error), function, error);
    rendering_ = true;
    return true;
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
    targetResources_.clear();
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
    if (!ours->Alive()) {
        AbxUnsupported(function, "a pipeline state used after its last reference was released");
    }
    if (ours->BindPoint() == VK_PIPELINE_BIND_POINT_GRAPHICS && type_ != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        AbxUnsupported(function, "a graphics pipeline state on a compute or copy command list");
    }
    pipelineBound_ = false;
    if (ours->Pipeline() != VK_NULL_HANDLE) {
        Recorder(function).BindPipeline(ours->BindPoint(), ours->Pipeline());
        pipelineBound_ = true;
    }
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
        arguments.signature = ours;
        ApplyPending(arguments);
    }
}

bool CommandList::WriteConstants(RootArguments& arguments, UINT index, UINT offset, const std::uint32_t* values, UINT count) {
    if (arguments.signature == nullptr) return false;
    const std::vector<RootParameterLayout>& parameters = arguments.signature->Layout().parameters;
    if (index >= parameters.size() || parameters[index].type != D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) return false;
    const RootParameterLayout& parameter = parameters[index];
    if (offset > parameter.constants) return false;
    if (count > parameter.constants - offset) {
        const std::uint32_t total = arguments.signature->Layout().words;
        const std::uint32_t start = parameter.firstWord + offset;
        const std::uint32_t kept = start < total ? (std::min)(count, total - start) : 0u;
        std::memcpy(arguments.words.data() + start, values, static_cast<std::size_t>(kept) * sizeof(std::uint32_t));
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            AbxLog("d3d12_x", "root constants that overrun their parameter (%u values at offset %u of a %u-value parameter) continue into the following root words, as console user-data registers "
                              "would, and values past the root signature's %u words are dropped",
                   count, offset, parameter.constants, total);
        }
        return true;
    }
    std::memcpy(arguments.words.data() + parameter.firstWord + offset, values, static_cast<std::size_t>(count) * sizeof(std::uint32_t));
    return true;
}

void CommandList::ApplyPending(RootArguments& arguments) {
    std::vector<PendingConstants> kept;
    for (const PendingConstants& pending : arguments.pending) {
        if (!WriteConstants(arguments, pending.index, pending.offset, pending.values.data(), static_cast<UINT>(pending.values.size()))) kept.push_back(pending);
    }
    arguments.pending = std::move(kept);
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
    Recorder(function);
    if (count == 0) return;
    if (data == nullptr) AbxUnsupported(function, "root constants without data");
    const auto* values = static_cast<const std::uint32_t*>(data);
    if (WriteConstants(arguments, index, offset, values, count)) return;
    for (auto found = arguments.pending.begin(); found != arguments.pending.end();) {
        if (found->index == index && found->offset == offset) found = arguments.pending.erase(found);
        else ++found;
    }
    arguments.pending.push_back({index, offset, std::vector<std::uint32_t>(values, values + count)});
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "root constants that do not fit the bound root signature (parameter %u, %u values at offset %u) are kept until a root signature or pipeline they fit is bound, "
                          "as console root arguments are registers written in any order",
               index, count, offset);
    }
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
    indexView_ = {};
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
    if (!PrepareDraw(function)) return;
    Recorder(function).Draw(vertices, instances, firstVertex, firstInstance);
    if (TracingFrame()) {
        std::string text;
        for (std::size_t slot = 0; slot < traceViews_.size(); ++slot) {
            const D3D12_VERTEX_BUFFER_VIEW& view = traceViews_[slot];
            if (view.BufferLocation == 0) continue;
            char line[128];
            std::snprintf(line, sizeof(line), " vb%zu 0x%llx size %u stride %u:", slot, static_cast<unsigned long long>(view.BufferLocation), view.SizeInBytes,
                          view.StrideInBytes);
            text += line;
            MEMORY_BASIC_INFORMATION region{};
            const auto* data = reinterpret_cast<const float*>(view.BufferLocation + static_cast<UINT64>(firstVertex) * view.StrideInBytes);
            if (VirtualQuery(data, &region, sizeof(region)) == sizeof(region) && region.State == MEM_COMMIT) {
                const std::size_t floats = (std::min)(static_cast<std::size_t>(vertices) * view.StrideInBytes / 4, std::size_t{24});
                for (std::size_t index = 0; index < floats; ++index) {
                    std::snprintf(line, sizeof(line), " %g", static_cast<double>(data[index]));
                    text += line;
                }
            } else {
                text += " (not readable)";
            }
        }
        AbxLog("d3d12_x", "TRACE   DrawInstanced(%u vertices, %u instances, first %u, %u)%s", vertices, instances, firstVertex, firstInstance, text.c_str());
    }
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) ReportRootState(graphics_, "first DrawInstanced");
}

void STDMETHODCALLTYPE CommandList::DrawIndexedInstanced(UINT indices, UINT instances, UINT firstIndex, INT baseVertex, UINT firstInstance) {
    const char* function = "ID3D12GraphicsCommandList::DrawIndexedInstanced";
    if (!PrepareDraw(function)) return;
    Recorder(function).DrawIndexed(indices, instances, firstIndex, baseVertex, firstInstance);
    if (TracingFrame()) {
        static std::atomic<int> traced{0};
        if (traced++ < 3) {
            ReportRootState(graphics_, "traced DrawIndexedInstanced");
            if (const GraphicsDescription* description = pipeline_->Description()) {
                const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d = description->desc;
                const D3D12_DEPTH_STENCIL_DESC& z = d.DepthStencilState;
                const D3D12_RENDER_TARGET_BLEND_DESC& b = d.BlendState.RenderTarget[0];
                AbxLog("d3d12_x",
                       "TRACE   state: depth %d write %d func %d | stencil %d read 0x%x write 0x%x front fail %d depthfail %d pass %d func %d | blend %d src %d dst %d op %d mask 0x%x | cull %d "
                       "fill %d | rt f%d dsv f%d samples %u",
                       z.DepthEnable, z.DepthWriteMask, z.DepthFunc, z.StencilEnable, z.StencilReadMask, z.StencilWriteMask, z.FrontFace.StencilFailOp, z.FrontFace.StencilDepthFailOp,
                       z.FrontFace.StencilPassOp, z.FrontFace.StencilFunc, b.BlendEnable, b.SrcBlend, b.DestBlend, b.BlendOp, b.RenderTargetWriteMask, d.RasterizerState.CullMode,
                       d.RasterizerState.FillMode, d.RTVFormats[0], d.DSVFormat, d.SampleDesc.Count);
                std::string elements;
                for (std::size_t index = 0; index < description->inputs.size(); ++index) {
                    const D3D12_INPUT_ELEMENT_DESC& element = description->inputs[index];
                    char line[128];
                    std::snprintf(line, sizeof(line), " [%s%u f%d slot %u offset %u class %d]", index < description->semantics.size() ? description->semantics[index].c_str() : "?",
                                  element.SemanticIndex, static_cast<int>(element.Format), element.InputSlot, element.AlignedByteOffset, static_cast<int>(element.InputSlotClass));
                    elements += line;
                }
                AbxLog("d3d12_x", "TRACE   input layout:%s", elements.c_str());
            }
            std::string text;
            for (std::size_t slot = 0; slot < traceViews_.size(); ++slot) {
                const D3D12_VERTEX_BUFFER_VIEW& view = traceViews_[slot];
                if (view.BufferLocation == 0) continue;
                char line[96];
                std::snprintf(line, sizeof(line), " vb%zu 0x%llx size %u stride %u:", slot, static_cast<unsigned long long>(view.BufferLocation), view.SizeInBytes, view.StrideInBytes);
                text += line;
                MEMORY_BASIC_INFORMATION region{};
                const auto* data = reinterpret_cast<const std::uint32_t*>(view.BufferLocation);
                if (VirtualQuery(data, &region, sizeof(region)) == sizeof(region) && region.State == MEM_COMMIT) {
                    for (std::size_t index = 0; index < (std::min)(static_cast<std::size_t>(view.StrideInBytes) * 4 / 4, std::size_t{24}); ++index) {
                        std::snprintf(line, sizeof(line), " %08x", data[index]);
                        text += line;
                    }
                }
            }
            AbxLog("d3d12_x", "TRACE   DrawIndexedInstanced(%u indices, %u instances, first %u, base %d)%s", indices, instances, firstIndex, baseVertex, text.c_str());
        }
    }
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) ReportRootState(graphics_, "first DrawIndexedInstanced");
}

void CommandList::DispatchMesh(UINT x, UINT y, UINT z) {
    const char* function = "D3D12 command list slot 129 (DispatchMesh)";
    if (!PrepareDraw(function)) return;
    const std::uint32_t* limits = context_.device.MeshMaxGroups();
    const std::uint64_t total = std::uint64_t{x} * y * z;
    if (x > limits[0] || y > limits[1] || z > limits[2] || total > context_.device.MeshMaxTotal()) {
        ++Stats().skippedOther;
        static std::atomic<std::uint64_t> over{0};
        const std::uint64_t count = ++over;
        if (count == 1 || (count & (count - 1)) == 0) {
            AbxLog("d3d12_x", "a mesh dispatch of %u x %u x %u groups is past the device's mesh limits (%u x %u x %u, %u in all) and is skipped (%llu so far)", x, y, z, limits[0],
                   limits[1], limits[2], context_.device.MeshMaxTotal(), static_cast<unsigned long long>(count));
        }
        return;
    }
    std::string error;
    Check(Recorder(function).DrawMesh(x, y, z, error), function, error);
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "mesh dispatches run as Vulkan mesh shader draws, with the mesh stage's root words moved from s16 to s8 where the pixel stage reads them (first: %u x %u x %u)", x,
               y, z);
        ReportRootState(graphics_, "first DispatchMesh");
    }
}

void STDMETHODCALLTYPE CommandList::Dispatch(UINT x, UINT y, UINT z) {
    const char* function = "ID3D12GraphicsCommandList::Dispatch";
    EndPass();
    if (pipeline_ == nullptr || pipeline_->BindPoint() != VK_PIPELINE_BIND_POINT_COMPUTE) {
        AbxUnsupported(function, pipeline_ == nullptr ? "a dispatch with no pipeline state set on this command list since its last reset"
                                                      : ("a dispatch while a graphics pipeline state is set: " + std::string(pipeline_->Shaders().empty() ? pipeline_->UnbuiltReason() : pipeline_->Shaders().front())).c_str());
    }
    if (SkippedWork(function)) return;
    PushUserData(compute_, kComputeFirstRootSgpr, function);
    BindTitleSet(VK_PIPELINE_BIND_POINT_COMPUTE);
    Recorder(function).Dispatch(x, y, z);
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) ReportRootState(compute_, "first Dispatch");
}

void CommandList::CopyMemoryToMemory(std::uint64_t destination, std::uint64_t source, std::uint64_t bytes, const char* function) {
    if (context_.addresses == nullptr) AbxUnsupported(function, "a memory copy on a device without a title address space");
    if (bytes == 0) return;
    if (destination < source + bytes && source < destination + bytes) AbxUnsupported(function, "a memory copy whose source and destination overlap");
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "CopyMemoryToMemoryX copies between title addresses on the GPU (first: 0x%llx -> 0x%llx, %llu bytes)", static_cast<unsigned long long>(source),
               static_cast<unsigned long long>(destination), static_cast<unsigned long long>(bytes));
    }
    std::string error;
    OrderCopy(function);
    const auto locate = [&](std::uint64_t address, std::uint64_t wanted, bool write, std::uint64_t& available) {
        return LocateTitleMemory(address, wanted, write, available, bytes, function);
    };
    while (bytes != 0) {
        std::uint64_t targetBytes = 0;
        std::uint64_t originBytes = 0;
        const std::optional<TitleAddressSpace::Location> target = locate(destination, bytes, true, targetBytes);
        const std::optional<TitleAddressSpace::Location> origin = locate(source, bytes, false, originBytes);
        const std::uint64_t piece = (std::min)({bytes, targetBytes, originBytes});
        Check(Recorder(function).CopyBufferRegion(BufferRef{target->buffer, target->offset, piece}, 0, BufferRef{origin->buffer, origin->offset, piece}, 0, piece, error), function,
              error);
        destination += piece;
        source += piece;
        bytes -= piece;
    }
}

std::optional<TitleAddressSpace::Location> CommandList::LocateTitleMemory(std::uint64_t address, std::uint64_t wanted, bool write, std::uint64_t& available, std::uint64_t bytes,
                                                                         const char* function) {
    std::optional<TitleAddressSpace::Location> found = context_.addresses->LocateRun(address, available);
    if (found && available != 0) return found;
    MEMORY_BASIC_INFORMATION region{};
    const bool queried = VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) == sizeof(region);
    const DWORD access = write ? (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)
                               : (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
    if (!queried || region.State != MEM_COMMIT || (region.Protect & access) == 0 || (region.Protect & PAGE_GUARD) != 0) {
        char text[260];
        std::snprintf(text, sizeof(text), "a memory copy of %llu bytes whose %s at title address 0x%llx is in no GPU-visible title memory (state 0x%lx, protection 0x%lx, type 0x%lx)",
                      static_cast<unsigned long long>(bytes), write ? "destination" : "source", static_cast<unsigned long long>(address),
                      queried ? region.State : 0ul, queried ? region.Protect : 0ul, queried ? region.Type : 0ul);
        AbxUnsupported(function, text);
    }
    const std::uint64_t regionEnd = reinterpret_cast<std::uint64_t>(region.BaseAddress) + region.RegionSize;
    const std::uint64_t span = (std::min)(wanted, regionEnd - address);
    std::string importError;
    if (!context_.addresses->Import(address, span, importError)) {
        AbxUnsupported(function, ("importing title memory for a memory copy failed: " + importError).c_str());
    }
    static std::atomic<bool> imported{false};
    if (!imported.exchange(true)) {
        AbxLog("d3d12_x", "CopyMemoryToMemoryX reaches title memory the GPU had not been given (first: 0x%llx, %llu bytes, protection 0x%lx); it is imported for the GPU, as all title memory is GPU-visible on the console",
               static_cast<unsigned long long>(address), static_cast<unsigned long long>(span), region.Protect);
    }
    found = context_.addresses->LocateRun(address, available);
    if (!found || available == 0) AbxUnsupported(function, "title memory imported for a memory copy is still not located");
    return found;
}

void CommandList::FillTitleMemory(std::uint64_t destination, std::uint64_t bytes, const std::uint8_t (&pattern)[16], std::uint32_t unit, const char* function) {
    if (context_.addresses == nullptr) AbxUnsupported(function, "a memory fill on a device without a title address space");
    if (bytes == 0) return;
    if (bytes % unit != 0 || destination % 4 != 0) AbxUnsupported(function, "a memory fill that is not a whole number of values or not 4-byte aligned");
    for (std::size_t index = 4; index < 16; ++index) {
        if (pattern[index] != pattern[index % 4]) AbxUnsupported(function, "a fill pattern that does not repeat every 4 bytes (only 32-bit repeating fills are recorded)");
    }
    std::uint32_t value = 0;
    std::memcpy(&value, pattern, sizeof(value));
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "FillMemoryWith*BitValueX fills title memory on the GPU (first: 0x%llx, %llu bytes, value 0x%08x)", static_cast<unsigned long long>(destination),
               static_cast<unsigned long long>(bytes), value);
    }
    std::string error;
    OrderCopy(function);
    while (bytes != 0) {
        std::uint64_t targetBytes = 0;
        const std::optional<TitleAddressSpace::Location> target = LocateTitleMemory(destination, bytes, true, targetBytes, bytes, function);
        const std::uint64_t piece = (std::min)(bytes, targetBytes) & ~std::uint64_t{3};
        if (piece == 0) AbxUnsupported(function, "a memory fill whose title memory run is shorter than 4 bytes");
        Check(Recorder(function).FillBuffer(BufferRef{target->buffer, target->offset, piece}, piece, value, error), function, error);
        destination += piece;
        bytes -= piece;
    }
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
    while (bytes != 0) {
        const std::optional<BufferRef> target = to->BufferSpan(destinationOffset, bytes);
        const std::optional<BufferRef> origin = from->BufferSpan(sourceOffset, bytes);
        if (!target || !origin || target->size == 0 || origin->size == 0) {
            char text[200];
            std::snprintf(text, sizeof(text), "a buffer copy of %llu bytes whose %s (title address 0x%llx + 0x%llx) is in no GPU-visible title memory", static_cast<unsigned long long>(bytes),
                          (!target || target->size == 0) ? "destination" : "source",
                          static_cast<unsigned long long>((!target || target->size == 0) ? to->TitleAddress() : from->TitleAddress()),
                          static_cast<unsigned long long>((!target || target->size == 0) ? destinationOffset : sourceOffset));
            AbxUnsupported(function, text);
        }
        const UINT64 piece = (std::min)(target->size, origin->size);
        Check(Recorder(function).CopyBufferRegion(*target, 0, *origin, 0, piece, error), function, error);
        destinationOffset += piece;
        sourceOffset += piece;
        bytes -= piece;
    }
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
    if (toTexture && !to->IsBuffer() && to->Texture().desc.Width >= 1024) {
        static std::atomic<int> diag{0};
        if (diag++ < 30) {
            AbxLog("d3d12_x", "DIAG copy into texture %p (%llux%u f%d) sub %u from %s %p footprint f%d %ux%u rowpitch %u offset %llu", static_cast<void*>(to->Texture().image),
                   static_cast<unsigned long long>(to->Texture().desc.Width), to->Texture().desc.Height, static_cast<int>(to->Texture().desc.Format), destination->SubresourceIndex,
                   fromTexture ? "texture" : "buffer", static_cast<void*>(from), static_cast<int>(source->PlacedFootprint.Footprint.Format), source->PlacedFootprint.Footprint.Width,
                   source->PlacedFootprint.Footprint.Height, source->PlacedFootprint.Footprint.RowPitch, static_cast<unsigned long long>(source->PlacedFootprint.Offset));
        }
    }
    if (toTexture != to->IsBuffer() && fromTexture != from->IsBuffer()) {
        if (toTexture && fromTexture) {
            const UINT toSub = destination->SubresourceIndex;
            const UINT fromSub = source->SubresourceIndex;
            if (CopyNeedsBuffer(to->Texture(), toSub, from->Texture(), fromSub)) {
                const D3D12_RESOURCE_DESC& sourceDesc = from->Texture().desc;
                const VkDeviceSize bytes = sourceDesc.Width * sourceDesc.Height * (sourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? sourceDesc.DepthOrArraySize : 1u) * 4ull;
                VkBuffer scratch = VK_NULL_HANDLE;
                if (allocator_ == nullptr || !allocator_->Scratch(bytes, scratch, error)) AbxUnsupported(function, ("a depth and color texture copy without a scratch buffer: " + error).c_str());
                static std::atomic<bool> reported{false};
                if (!reported.exchange(true)) {
                    AbxLog("d3d12_x", "texture copies between a depth or stencil plane and a color or other depth format go through a scratch buffer (first: format %d to format %d)",
                           static_cast<int>(sourceDesc.Format), static_cast<int>(to->Texture().desc.Format));
                }
                Check(Recorder(function).CopyTextureThroughBuffer(to->Texture(), StateOf(to, toSub, D3D12_RESOURCE_STATE_COPY_DEST), toSub, x, y, z, from->Texture(),
                                                                  StateOf(from, fromSub, D3D12_RESOURCE_STATE_COPY_SOURCE), fromSub, box, BufferRef{scratch, 0, bytes}, error),
                      function, error);
                return;
            }
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

namespace {

std::optional<UINT> StoredSubresource(const Resource& resource, UINT titleSubresource) {
    const UINT skip = resource.SkippedMips();
    if (skip == 0) return titleSubresource;
    const TextureRef& texture = resource.Texture();
    const UINT stored = texture.layout.info.mipLevels;
    const UINT title = stored + skip;
    const UINT layers = texture.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : texture.desc.DepthOrArraySize;
    const UINT mip = titleSubresource % title;
    const UINT rest = titleSubresource / title;
    if (mip < skip) return std::nullopt;
    return (mip - skip) + (rest % layers) * stored + (rest / layers) * stored * layers;
}

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
                const std::optional<UINT> stored = StoredSubresource(*resource, subresource);
                if (!stored) {
                    states_[{resource, subresource}] = barrier.Transition.StateAfter;
                    continue;
                }
                const std::optional<Subresource> split = SplitSubresource(texture.desc, *stored, texture.layout.info.mipLevels);
                const std::optional<VkImageAspectFlags> aspect = split ? PlaneAspect(texture.layout.format.aspects, split->plane) : std::nullopt;
                if (!aspect) {
                    AbxUnsupported(function, ("a barrier on subresource " + std::to_string(subresource) + " outside the texture (" + std::to_string(texture.layout.info.mipLevels) +
                                              " stored mips, " + std::to_string(resource->SkippedMips()) + " left out, " + std::to_string(texture.desc.DepthOrArraySize) + " layers)")
                                                 .c_str());
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
    indexView_ = *view;
}

void* CommandList::AllocateTransient(std::size_t bytes, std::size_t alignment, const char* function) {
    if (closed_ || allocator_ == nullptr) AbxUnsupported(function, "transient memory from a closed command list");
    if (context_.addresses == nullptr) AbxUnsupported(function, "transient memory on a device without a title address space");
    std::string error;
    CommandAllocator::TransientBlock* created = nullptr;
    const std::uintptr_t address = allocator_->Transient(bytes, alignment, created, error);
    if (address == 0) AbxUnsupported(function, error.c_str());
    if (created != nullptr) {
        if (!context_.addresses->Import(created->base, created->size, error)) AbxUnsupported(function, ("importing transient command list memory for the GPU failed: " + error).c_str());
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            AbxLog("d3d12_x", "command list slot 148 allocates transient GPU-visible memory (size, alignment) from the command allocator: blocks of 4 MiB below 1 TiB, reused after the allocator's Reset (first: %zu bytes, alignment %zu, at 0x%llx)",
                   bytes, alignment, static_cast<unsigned long long>(address));
        }
    }
    return reinterpret_cast<void*>(address);
}

void CommandList::DrawIndexedAt(std::uint64_t address, DXGI_FORMAT format, UINT count, const char* function) {
    UINT stride = 0;
    if (format == DXGI_FORMAT_R16_UINT) stride = 2;
    else if (format == DXGI_FORMAT_R32_UINT) stride = 4;
    else AbxUnsupported(function, ("index format " + std::to_string(static_cast<int>(format))).c_str());
    if (count == 0) return;
    if (address == 0) AbxUnsupported(function, "a null index address");
    const D3D12_INDEX_BUFFER_VIEW previous = indexView_;
    const D3D12_INDEX_BUFFER_VIEW view{address, count * stride, format};
    IASetIndexBuffer(&view);
    DrawIndexedInstanced(count, 1, 0, 0, 0);
    if (previous.BufferLocation != 0) IASetIndexBuffer(&previous);
    indexView_ = previous;
}

void STDMETHODCALLTYPE CommandList::IASetVertexBuffers(UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW* views) {
    const char* function = "ID3D12GraphicsCommandList::IASetVertexBuffers";
    if (views == nullptr) {
        return;
    }
    std::string error;
    for (UINT index = 0; index < count; ++index) {
        if (start + index < traceViews_.size()) traceViews_[start + index] = views[index];
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
    std::vector<Resource*> targetResources;
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
        targetResources.push_back(record.kind == HeapEntry::RenderTarget ? record.resource : nullptr);
    }
    std::optional<AttachmentRef> depth;
    if (depthStencil != nullptr) {
        const DescriptorRecord& record = Attachment(*depthStencil, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, function);
        if (record.kind == HeapEntry::DepthStencil) depth = record.attachment;
    }
    targetsSet_ = true;
    emptyTargets_ = 0;
    for (const AttachmentRef& color : colors) emptyTargets_ += color.view == VK_NULL_HANDLE ? 1u : 0u;
    depthHandleGiven_ = depthStencil != nullptr;
    renderTargets_ = std::move(colors);
    targetResources_ = std::move(targetResources);
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
namespace {

QueryHeap* OurQueryHeap(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, const char* function) {
    auto* ours = dynamic_cast<QueryHeap*>(heap);
    if (ours == nullptr) AbxUnsupported(function, "a query heap that AnyBox did not create");
    if (QueryHeap::ResultBytes(type) == 0) AbxUnsupported(function, ("query type " + std::to_string(static_cast<int>(type))).c_str());
    return ours;
}

}

void STDMETHODCALLTYPE CommandList::BeginQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
    const char* function = "ID3D12GraphicsCommandList::BeginQuery";
    QueryHeap* ours = OurQueryHeap(heap, type, function);
    if (type == D3D12_QUERY_TYPE_TIMESTAMP || index >= ours->Desc().Count) AbxUnsupported(function, "a timestamp query or an index past the query heap");
}

void STDMETHODCALLTYPE CommandList::EndQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
    const char* function = "ID3D12GraphicsCommandList::EndQuery";
    QueryHeap* ours = OurQueryHeap(heap, type, function);
    std::uint8_t result[sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)] = {};
    const std::uint32_t bytes = QueryHeap::ResultBytes(type);
    std::uint64_t value = 0;
    if (type == D3D12_QUERY_TYPE_OCCLUSION) {
        value = 0x10000;
    } else if (type == D3D12_QUERY_TYPE_BINARY_OCCLUSION) {
        value = 1;
    } else if (type == D3D12_QUERY_TYPE_TIMESTAMP) {
        const float period = context_.device.Properties().limits.timestampPeriod;
        LARGE_INTEGER now{};
        LARGE_INTEGER ticks{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&ticks);
        if (!(period > 0.0f) || ticks.QuadPart == 0) AbxUnsupported(function, "a timestamp query without a GPU timestamp period");
        const long double seconds = static_cast<long double>(now.QuadPart) / static_cast<long double>(ticks.QuadPart);
        value = static_cast<std::uint64_t>(seconds * 1.0e9L / static_cast<long double>(period));
    }
    std::memcpy(result, &value, sizeof(value));
    if (!ours->Store(index, result, bytes)) AbxUnsupported(function, "an index past the query heap");
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "queries are answered when they are recorded: occlusion queries report visible, timestamps the CPU clock in GPU timestamp units, statistics zero "
                          "(first: type %d)", static_cast<int>(type));
    }
}

void STDMETHODCALLTYPE CommandList::ResolveQueryData(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT start, UINT count, ID3D12Resource* destination, UINT64 offset) {
    const char* function = "ID3D12GraphicsCommandList::ResolveQueryData";
    QueryHeap* ours = OurQueryHeap(heap, type, function);
    if (count == 0) return;
    if (start > ours->Desc().Count || count > ours->Desc().Count - start) AbxUnsupported(function, "queries past the query heap");
    Resource* to = Ours(destination, function);
    if (!to->IsBuffer() || offset % 8 != 0) AbxUnsupported(function, "a destination that is not a buffer or an offset that is not 8-byte aligned");
    const std::uint32_t stride = QueryHeap::ResultBytes(type);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(count) * stride);
    for (UINT index = 0; index < count; ++index) ours->Load(start + index, data.data() + static_cast<std::size_t>(index) * stride, stride);
    std::string error;
    OrderCopy(function);
    UINT64 done = 0;
    while (done != data.size()) {
        const std::optional<BufferRef> target = to->BufferSpan(offset + done, data.size() - done);
        if (!target || target->size < 4) AbxUnsupported(function, "a destination buffer range in no GPU-visible memory");
        const UINT64 piece = (std::min)(target->size, static_cast<UINT64>(data.size() - done)) & ~UINT64{3};
        Check(Recorder(function).UpdateBuffer(*target, data.data() + done, piece, error), function, error);
        done += piece;
    }
}

void STDMETHODCALLTYPE CommandList::SetPredication(ID3D12Resource* buffer, UINT64 offset, D3D12_PREDICATION_OP operation) {
    static std::atomic<bool> reported{false};
    if (buffer != nullptr && !reported.exchange(true)) {
        AbxLog("d3d12_x", "SetPredication is not applied, so predicated work always runs (first: offset %llu, operation %d)", static_cast<unsigned long long>(offset),
               static_cast<int>(operation));
    }
}
void STDMETHODCALLTYPE CommandList::ExecuteIndirect(ID3D12CommandSignature* signature, UINT maxCount, ID3D12Resource* arguments, UINT64 argumentOffset, ID3D12Resource* count,
                                                     UINT64 countOffset) {
    const char* function = "ID3D12GraphicsCommandList::ExecuteIndirect";
    auto* ours = dynamic_cast<CommandSignature*>(signature);
    if (ours == nullptr) AbxUnsupported(function, "a command signature that AnyBox did not create");
    if (!ours->Signature()) AbxUnsupported(function, ("a command signature AnyBox cannot execute: " + ours->Unsupported()).c_str());
    const IndirectSignature& indirect = *ours->Signature();
    if (maxCount == 0) return;
    Resource* argumentBuffer = Ours(arguments, function);
    if (argumentBuffer == nullptr || !argumentBuffer->IsBuffer()) AbxUnsupported(function, "indirect arguments that are not in a buffer");
    const UINT64 span = static_cast<UINT64>(indirect.stride) * (maxCount - 1) + IndirectArgumentBytes(indirect.kind);
    const std::optional<BufferRef> argumentRef = argumentBuffer->BufferSpan(argumentOffset, span);
    if (!argumentRef || argumentRef->size < span) AbxUnsupported(function, "indirect arguments that are not one GPU-visible range");
    std::optional<BufferRef> countRef;
    if (count != nullptr) {
        Resource* countBuffer = Ours(count, function);
        if (countBuffer == nullptr || !countBuffer->IsBuffer()) AbxUnsupported(function, "an indirect count that is not in a buffer");
        countRef = countBuffer->BufferSpan(countOffset, 4);
        if (!countRef || countRef->size < 4) AbxUnsupported(function, "an indirect count that is not GPU-visible");
    }
    std::optional<BufferRef> checkedArguments;
    if (indirect.kind == IndirectKind::Dispatch) {
        EndPass();
        if (pipeline_ == nullptr || pipeline_->BindPoint() != VK_PIPELINE_BIND_POINT_COMPUTE) AbxUnsupported(function, "an indirect dispatch without a compute pipeline state set");
        if (SkippedWork(function)) return;
        if (IndirectDispatchGuarded()) {
            ++Stats().skippedOther;
            static std::atomic<std::uint64_t> guarded{0};
            const std::uint64_t total = ++guarded;
            if (total == 1 || (total & (total - 1)) == 0) {
                AbxLog("d3d12_x", "an indirect compute dispatch is skipped because compute work was skipped in this frame or the one before: its group counts may come from that work, "
                                  "and garbage counts hang the GPU until Windows resets it (%llu so far, this one %s; d3d12_x.indirect_dispatch_guard = 1 asked for this)",
                       static_cast<unsigned long long>(total), pipeline_->Shaders().empty() ? "(no shader)" : pipeline_->Shaders().front().substr(0, 22).c_str());
            }
            return;
        }
        if (IndirectClamp* clamp = IndirectClamp::For(context_.device)) {
            std::string clampError;
            const std::optional<ClampedArguments> checked = clamp->RecordDispatches(commands_, argumentRef->buffer, argumentRef->offset, maxCount, indirect.stride, false, clampError);
            Check(checked.has_value(), function, clampError);
            checkedArguments = BufferRef{checked->buffer, checked->offset, checked->size};
            Recorder(function).BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_->Pipeline());
            pipelineBound_ = true;
            computeSetBound_ = false;
        }
        PushUserData(compute_, kComputeFirstRootSgpr, function);
        BindTitleSet(VK_PIPELINE_BIND_POINT_COMPUTE);
    } else if (indirect.kind == IndirectKind::DispatchMesh) {
        if (pipeline_ == nullptr || pipeline_->BindPoint() != VK_PIPELINE_BIND_POINT_GRAPHICS) AbxUnsupported(function, "an indirect mesh dispatch without a graphics pipeline state set");
        if (IndirectClamp* clamp = IndirectClamp::For(context_.device)) {
            EndPass();
            std::string clampError;
            const std::optional<ClampedArguments> checked = clamp->RecordDispatches(commands_, argumentRef->buffer, argumentRef->offset, maxCount, indirect.stride, true, clampError);
            Check(checked.has_value(), function, clampError);
            checkedArguments = BufferRef{checked->buffer, checked->offset, checked->size};
            computeSetBound_ = false;
        }
        if (!PrepareDraw(function)) return;
    } else if (!PrepareDraw(function)) {
        return;
    }
    std::string error;
    Check(Recorder(function).ExecuteIndirect(indirect, maxCount, checkedArguments ? *checkedArguments : *argumentRef, 0, countRef ? &*countRef : nullptr, 0, error), function, error);
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "ExecuteIndirect runs as Vulkan indirect %s (first: up to %u commands, stride %u%s)",
               indirect.kind == IndirectKind::Dispatch ? "dispatches" : indirect.kind == IndirectKind::DispatchMesh ? "mesh draws" : "draws", maxCount,
               indirect.stride, count != nullptr ? ", count from a buffer" : "");
    }
}

}
