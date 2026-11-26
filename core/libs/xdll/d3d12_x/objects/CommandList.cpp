#include "CommandList.hpp"
#include "CommandSignature.hpp"
#include "FrameDump.hpp"
#include "FrameTiming.hpp"
#include "GpuProfile.hpp"
#include "IndirectClamp.hpp"
#include "TableCopy.hpp"
#include "DescriptorWriter.hpp"
#include "DescriptorHeap.hpp"
#include "LiveStats.hpp"
#include "QueryHeap.hpp"
#include "TextureReadback.hpp"

#include "AbxApi.hpp"
#include "BindlessSet.hpp"
#include "BufferFormatSpec.hpp"
#include "CopyLayout.hpp"
#include "ResourceState.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <vector>

namespace abx::d3d12x {

namespace {

constexpr UINT kAllSubresources = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

std::string TitleWordsText(const TitleAddressSpace* addresses, std::uint64_t address, std::uint64_t bytes) {
    constexpr std::uint64_t kPage = 0x10000;
    const std::uint64_t wanted = (std::min)((std::min)(bytes, std::uint64_t{32}), kPage - (address & (kPage - 1))) & ~std::uint64_t{3};
    if (wanted == 0) return "";
    std::uint8_t host[32] = {};
    if (addresses == nullptr || addresses->ReadForTrace(address, host, wanted) != wanted) return "; its first bytes are not readable from the CPU";
    std::string text = "; first words:";
    for (std::uint64_t at = 0; at < wanted; at += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, host + at, sizeof(word));
        char piece[16];
        std::snprintf(piece, sizeof(piece), " %08x", word);
        text += piece;
    }
    return text;
}

constexpr std::uint32_t kGraphicsFirstRootSgpr = 8;
constexpr std::uint32_t kComputeFirstRootSgpr = 4;
constexpr std::uint32_t kUserSgprs = 32;
constexpr std::uint32_t kPageTableWord = 32;
constexpr std::uint32_t kPushWords = 36;
constexpr std::uint32_t kThreadLimitSgpr = 31;
constexpr UINT kDispatchCountsThreads = 0x20;
constexpr float kDefaultBlendFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};

std::uint32_t HeapIndexing(const RootSignature* signature) {
    if (signature == nullptr) return 0;
    return (static_cast<std::uint32_t>(signature->Layout().flags) | signature->Layout().heapIndexing) & kHeapIndexingFlags;
}

std::uint32_t HeapBaseWord(const DescriptorHeap* heap, const char* function) {
    if (heap == nullptr) return 0;
    const std::uint64_t start = heap->GpuStart().ptr;
    if (start > 0xFFFFFFFFull) {
        AbxUnsupported(function, "a directly indexed descriptor heap above 4 GB, which a single user data word cannot hold");
    }
    return static_cast<std::uint32_t>(start);
}

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
    created->ProtectHead(true);
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
    finished_.clear();
    checkCopies_ = VK_NULL_HANDLE;
    checks_ = VK_NULL_HANDLE;
    checksWritten_ = false;
    std::copy(std::begin(kDefaultBlendFactor), std::end(kDefaultBlendFactor), blendFactor_.begin());
    stencilReference_ = 0;
    indexBuffer_.reset();
    vertexBuffers_.fill(std::nullopt);
    profileGeneration_ = GpuProfile::BeginList(commands, profileList_);
    allocator_ = pool;
    closed_ = false;
    targetsSet_ = false;
    emptyTargets_ = 0;
    depthHandleGiven_ = false;
    states_.clear();
    readbackWritten_.clear();
    copied_ = false;
    rendering_ = false;
    rectangles_ = false;
    viewports_.clear();
    scissors_.clear();
    topology_.reset();
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
    CloseChecksAhead();
    if (!rendering_) return;
    Recorder("ID3D12GraphicsCommandList").EndRendering();
    rendering_ = false;
}

std::vector<VkCommandBuffer> CommandList::CommandBuffers() const {
    std::vector<VkCommandBuffer> buffers = finished_;
    buffers.push_back(commands_);
    return buffers;
}

bool CommandList::OpenChecksAhead(const char* function) {
    IndirectClamp* clamp = IndirectClamp::For(context_.device);
    if (clamp == nullptr || allocator_ == nullptr || closed_) return false;
    const DeviceFunctions& vk = context_.device.Vk();
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    std::array<VkCommandBuffer, 3> made{};
    std::string error;
    for (VkCommandBuffer& buffer : made) {
        buffer = allocator_->Next(error);
        if (buffer == VK_NULL_HANDLE) AbxUnsupported(function, ("a command buffer for the indirect argument checks ahead of a render pass: " + error).c_str());
        if (vk.beginCommandBuffer(buffer, &begin) != VK_SUCCESS) AbxUnsupported(function, "vkBeginCommandBuffer failed for the indirect argument checks ahead of a render pass");
    }
    if (vk.endCommandBuffer(commands_) != VK_SUCCESS) AbxUnsupported(function, "vkEndCommandBuffer failed where the list is split for the indirect argument checks");
    finished_.push_back(commands_);
    finished_.push_back(made[0]);
    finished_.push_back(made[1]);
    commands_ = made[2];
    checkCopies_ = made[0];
    checks_ = made[1];
    checksWritten_ = false;
    clamp->BeginBatch(checkCopies_, checks_);
    ++Stats().clampBatches;
    ReplayState(function);
    return true;
}

void CommandList::CloseChecksAhead() {
    if (checks_ == VK_NULL_HANDLE) return;
    if (IndirectClamp* clamp = IndirectClamp::For(context_.device)) clamp->EndBatch(checks_);
    const DeviceFunctions& vk = context_.device.Vk();
    const bool ended = vk.endCommandBuffer(checkCopies_) == VK_SUCCESS && vk.endCommandBuffer(checks_) == VK_SUCCESS;
    checkCopies_ = VK_NULL_HANDLE;
    checks_ = VK_NULL_HANDLE;
    if (!ended) AbxUnsupported("ID3D12GraphicsCommandList", "vkEndCommandBuffer failed for the indirect argument checks ahead of a render pass");
}

void CommandList::ReplayState(const char* function) {
    pipelineBound_ = false;
    graphicsSetBound_ = false;
    computeSetBound_ = false;
    if (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    CommandRecorder recorder = Recorder(function);
    std::string error;
    if (!viewports_.empty()) Check(recorder.SetViewports(viewports_, error), function, error);
    if (!scissors_.empty()) Check(recorder.SetScissors(scissors_, error), function, error);
    if (topology_) Check(recorder.SetPrimitiveTopology(*topology_, false, error), function, error);
    recorder.SetBlendFactor(blendFactor_.data());
    recorder.SetStencilReference(stencilReference_);
    if (indexBuffer_) Check(recorder.SetIndexBuffer(indexBuffer_->first, indexBuffer_->second, error), function, error);
    for (UINT slot = 0; slot < vertexBuffers_.size(); ++slot) {
        if (vertexBuffers_[slot]) Check(recorder.SetVertexBuffers(slot, {*vertexBuffers_[slot]}, error), function, error);
    }
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

namespace {

constexpr std::uint32_t kCodeBaseWord = 34;
constexpr std::size_t kCodeBlockBytes = 4u << 20;
constexpr std::size_t kCodeAlignment = 256;

std::filesystem::path CodeBlobPath(const std::filesystem::path& module) {
    std::filesystem::path stem = module.stem();
    return module.parent_path().parent_path() / L"dxbc" / L"fssm" / stem.concat(L".fssm");
}

std::uint64_t PlaceCodeBlob(const std::filesystem::path& module, TitleAddressSpace& addresses, std::string& error) {
    static std::mutex lock;
    static std::map<std::filesystem::path, std::uint64_t> placed;
    static std::uintptr_t block = 0;
    static std::size_t blockUsed = 0;
    static std::size_t blockSize = 0;
    static std::uint64_t totalBytes = 0;
    const std::lock_guard guard(lock);
    const auto found = placed.find(module);
    if (found != placed.end()) return found->second;
    const std::filesystem::path path = CodeBlobPath(module);
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "its code blob " + path.string() + " cannot be opened";
        return 0;
    }
    const std::streamoff length = file.tellg();
    if (length < 16 || static_cast<std::uint64_t>(length) > kCodeBlockBytes) {
        error = "its code blob " + path.string() + " has an implausible size of " + std::to_string(length) + " bytes";
        return 0;
    }
    std::vector<char> bytes(static_cast<std::size_t>(length));
    file.seekg(0);
    if (!file.read(bytes.data(), length) || file.gcount() != length) {
        error = "its code blob " + path.string() + " could not be read in full";
        return 0;
    }
    if (std::memcmp(bytes.data(), "FSSM", 4) != 0) {
        error = "its code blob " + path.string() + " does not start with FSSM";
        return 0;
    }
    const std::size_t start = (blockUsed + kCodeAlignment - 1) & ~(kCodeAlignment - 1);
    if (block == 0 || start > blockSize || bytes.size() > blockSize - start) {
        const std::uintptr_t fresh = CommandAllocator::ReserveHigh(kCodeBlockBytes, error);
        if (fresh == 0) {
            error = "no memory for code blobs: " + error;
            return 0;
        }
        if (!addresses.Import(fresh, kCodeBlockBytes, error)) {
            VirtualFree(reinterpret_cast<void*>(fresh), 0, MEM_RELEASE);
            error = "importing code blob memory for the GPU failed: " + error;
            return 0;
        }
        block = fresh;
        blockSize = kCodeBlockBytes;
        blockUsed = 0;
    }
    const std::size_t at = (blockUsed + kCodeAlignment - 1) & ~(kCodeAlignment - 1);
    std::memcpy(reinterpret_cast<void*>(block + at), bytes.data(), bytes.size());
    blockUsed = at + bytes.size();
    const std::uint64_t address = block + at;
    placed.emplace(module, address);
    totalBytes += bytes.size();
    const std::size_t count = placed.size();
    if (count == 1 || (count & (count - 1)) == 0) {
        AbxLog("d3d12_x", "shaders that read constants from their own code get their whole code blob (the store's .fssm file) in GPU-visible memory, its address as the code base in "
                          "push words 34-35 (%zu blob(s) so far, %llu KiB; latest %s at 0x%llx)",
               count, static_cast<unsigned long long>(totalBytes >> 10), path.filename().string().c_str(), static_cast<unsigned long long>(address));
    }
    return address;
}

}

bool CommandList::ResolveCodeBase(const char* function, std::string& reason) {
    if (pipeline_->CodeBase() != 0) return true;
    const std::vector<std::filesystem::path>& modules = pipeline_->CodeBaseModules();
    if (modules.size() != 1) {
        reason = modules.empty() ? "its shaders read constants from their own code, but which stage does is not known"
                                 : "two of its stages read constants from their own code, and one push block holds only one code base";
        return false;
    }
    if (context_.addresses == nullptr) AbxUnsupported(function, "a code base on a device without a title address space");
    std::string error;
    const std::uint64_t address = PlaceCodeBlob(modules.front(), *context_.addresses, error);
    if (address == 0) {
        reason = "its shaders read constants from their own code, and " + error;
        return false;
    }
    pipeline_->SetCodeBase(address);
    return true;
}

namespace {

constexpr std::size_t kTrapPage = 0x1000;
std::size_t TrapBytes(std::size_t size) {
    return kTrapPage + ((size - sizeof(void*) + kTrapPage - 1) & ~(kTrapPage - 1));
}

}

void* CommandList::operator new(std::size_t size) {
    static_assert(alignof(CommandList) <= sizeof(void*));
    void* const base = VirtualAlloc(nullptr, TrapBytes(size), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (base == nullptr) throw std::bad_alloc();
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "DIAG command lists are placed with their vtable pointer as the last 8 bytes of a read-only page, so a stray write there faults in the writer "
                          "(anybox-crash-raw.txt has its frames)");
    }
    return static_cast<unsigned char*>(base) + kTrapPage - sizeof(void*);
}

void CommandList::operator delete(void* object) {
    if (object == nullptr) return;
    VirtualFree(static_cast<unsigned char*>(object) - (kTrapPage - sizeof(void*)), 0, MEM_RELEASE);
}

void CommandList::ProtectHead(bool readOnly) {
    void* const page = reinterpret_cast<unsigned char*>(this) - (kTrapPage - sizeof(void*));
    DWORD previous = 0;
    if (!VirtualProtect(page, kTrapPage, readOnly ? PAGE_READONLY : PAGE_READWRITE, &previous)) {
        AbxLog("d3d12_x", "DIAG the command list write trap could not change protection (error %lu)", GetLastError());
    }
}

void CommandList::BeforeDelete() {
    ProtectHead(false);
}

void CommandList::ReportDamage(const char* function) const {
    const auto* here = reinterpret_cast<const unsigned char*>(&context_);
    std::string dump;
    char text[96];
    for (std::ptrdiff_t at = -0x40; at < static_cast<std::ptrdiff_t>(sizeof(context_)) + 0x40; at += 8) {
        std::uint64_t word = 0;
        std::memcpy(&word, here + at, sizeof(word));
        std::snprintf(text, sizeof(text), " %+td=%016llx", at, static_cast<unsigned long long>(word));
        dump += text;
    }
    AbxLog("d3d12_x", "DIAG damaged command list %p in %s: alive tag 0x%08x (0x%08x when alive), context device %p window %p arena %p title memory %p bindless %p addresses %p; words around the context:%s",
           static_cast<const void*>(this), function, alive_, kAlive, static_cast<const void*>(&context_.device), static_cast<const void*>(&context_.window),
           static_cast<const void*>(&context_.arena), static_cast<const void*>(context_.titleMemory), static_cast<const void*>(context_.bindless),
           static_cast<const void*>(context_.addresses), dump.c_str());
}

void CommandList::PushUserData(RootArguments& arguments, std::uint32_t firstSgpr, const char* function, std::uint32_t threadLimit) {
    std::array<std::uint32_t, kPushWords> block{};
    UserDataBlock(arguments, firstSgpr, function, threadLimit, block.data());
    std::string error;
    Check(Recorder(function).SetPushConstants(block.data(), static_cast<std::uint32_t>(block.size() * sizeof(std::uint32_t)), error), function, error);
}

void CommandList::UserDataBlock(RootArguments& arguments, std::uint32_t firstSgpr, const char* function, std::uint32_t threadLimit, std::uint32_t* out) {
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
    const std::uint32_t indexing = HeapIndexing(arguments.signature);
    const bool resourceIndexed = (indexing & D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED) != 0;
    const bool samplerIndexed = (indexing & D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED) != 0;
    if (words > kUserSgprs - firstSgpr || (resourceIndexed ? 1u : 0u) + (samplerIndexed ? 1u : 0u) > kUserSgprs - firstSgpr - words) {
        AbxUnsupported(function, "a root signature whose words do not fit in the user SGPRs after the driver's reserved ones; the console's spill layout is not measured");
    }
    std::copy_n(arguments.words.begin(), words, block.begin() + firstSgpr);
    if (context_.addresses != nullptr) context_.addresses->MarkGpuRootWords(arguments.words.data(), words);
    if (threadLimit != 0) {
        if (firstSgpr + words + (resourceIndexed ? 1u : 0u) + (samplerIndexed ? 1u : 0u) > kThreadLimitSgpr) {
            AbxUnsupported(function, "a thread-count dispatch whose root signature fills the user SGPR that carries the thread limit");
        }
        block[kThreadLimitSgpr] = threadLimit;
    }
    if (indexing != 0) {
        std::uint32_t next = firstSgpr + words;
        const std::uint32_t resourceAt = resourceIndexed ? next++ : 0;
        const std::uint32_t samplerAt = samplerIndexed ? next++ : 0;
        if (resourceIndexed) block[resourceAt] = HeapBaseWord(shaderResourceHeap_, function);
        if (samplerIndexed) block[samplerAt] = HeapBaseWord(samplerHeap_, function);
        static std::atomic<std::uint32_t> reported{0};
        const std::uint32_t bit = 1u << (((indexing >> 10) & 3u) + (firstSgpr == kComputeFirstRootSgpr ? 4u : 0u));
        if ((reported.fetch_or(bit) & bit) == 0) {
            AbxLog("d3d12_x", "directly indexed descriptor heaps (root signature flags 0x%x, %u root words from s%u): resource heap base 0x%x %s%u, sampler heap base 0x%x %s%u, "
                              "right after the root words in that order, as the title's shaders read them (first by %s)",
                   indexing, words, firstSgpr, resourceIndexed ? block[resourceAt] : 0u, resourceIndexed ? "in s" : "not passed, s", resourceAt,
                   samplerIndexed ? block[samplerAt] : 0u, samplerIndexed ? "in s" : "not passed, s", samplerAt, function);
        }
    }
    if (context_.addresses == nullptr) {
        ReportDamage(function);
        AbxUnsupported(function, "a draw or dispatch on a device without a title address space");
    }
    const std::uint64_t table = context_.addresses->RootTable();
    block[kPageTableWord] = static_cast<std::uint32_t>(table);
    block[kPageTableWord + 1] = static_cast<std::uint32_t>(table >> 32);
    const std::uint64_t codeBase = pipeline_ != nullptr ? pipeline_->CodeBase() : 0;
    block[kCodeBaseWord] = static_cast<std::uint32_t>(codeBase);
    block[kCodeBaseWord + 1] = static_cast<std::uint32_t>(codeBase >> 32);
    std::copy(block.begin(), block.end(), out);
}

int CommandList::ChooseFormatVariant(const char* function, VkPipeline& variant, std::string& why) {
    enum { UseStored = 0, Ready = 1, Building = 2, Failed = 3 };
    static std::mutex toldLock;
    static std::set<std::string> told;
    const auto tell = [&](const char* what, const std::string& detail) {
        const std::string shader = pipeline_->Shaders().empty() ? std::string("(no shader)") : pipeline_->Shaders().front();
        const std::lock_guard guard(toldLock);
        if (told.size() < 64 && told.insert(shader + what).second) AbxLog("d3d12_x", "compute pipeline %s %s: %s", shader.c_str(), what, detail.c_str());
    };
    std::array<std::uint32_t, kPushWords> block{};
    UserDataBlock(compute_, kComputeFirstRootSgpr, function, 0, block.data());
    std::vector<std::uint32_t> words;
    for (const BufferFormatSource& source : pipeline_->FormatSources()) {
        const std::optional<std::uint64_t> address = BufferFormatAddress(source, block.data(), kUserSgprs);
        const std::uint8_t* host = nullptr;
        if (address) {
            UINT increment = 0;
            host = DescriptorHeap::AtGpuAddress(*address, increment);
            if (host == nullptr && context_.addresses != nullptr && context_.addresses->Contains(*address, sizeof(std::uint32_t))) host = context_.addresses->HostAt(*address);
        }
        if (host == nullptr) {
            char text[160];
            std::snprintf(text, sizeof(text), "word 3 of a buffer descriptor (title address 0x%llx) is not readable by the CPU when the dispatch is recorded",
                          static_cast<unsigned long long>(address.value_or(0)));
            tell("uses the module as stored", text);
            pipeline_->StartGenericBuild();
            return UseStored;
        }
        std::uint32_t word = 0;
        std::memcpy(&word, host, sizeof(word));
        words.push_back(word);
    }
    bool pending = false;
    bool generic = false;
    variant = pipeline_->FormatVariant(words, pending, generic, why);
    if (variant != VK_NULL_HANDLE) return Ready;
    if (pending) return Building;
    if (generic) {
        tell("uses the module as stored", why);
        pipeline_->StartGenericBuild();
        return UseStored;
    }
    return Failed;
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

bool DrawsWaitForPipelines() {
    static const bool enabled = [] {
        const char* value = AbxSetting("d3d12_x.draws_wait_for_pipelines");
        return value == nullptr || !(value[0] == '0' && value[1] == 0);
    }();
    return enabled;
}

bool ChecksAheadOfPass() {
    static const bool enabled = [] {
        const char* value = AbxSetting("d3d12_x.indirect_checks_ahead");
        const bool on = value != nullptr && value[0] == '1' && value[1] == 0;
        if (on) {
            AbxLog("d3d12_x", "indirect mesh draws inside a render pass have their arguments checked ahead of that pass, in command buffers of their own, so the pass is not ended "
                              "and begun again around each one (d3d12_x.indirect_checks_ahead = 1; off unless set)");
        }
        return on;
    }();
    return enabled;
}

void NoteChecksRestarted(const PipelineStateObject& pipeline) {
    const std::uint64_t total = ++Stats().clampBatchesRestarted;
    if (total == 1 || (total & (total - 1)) == 0) {
        AbxLog("d3d12_x", "an indirect mesh draw has its arguments checked ahead of a new render pass, not with the earlier ones of its pass, because a draw since those checks "
                          "may have written memory (its pipeline stores to buffers or images), which could be these arguments; the pass is ended and begun again here, as "
                          "with checks in place (%llu so far, this draw %s)",
               static_cast<unsigned long long>(total), pipeline.Shaders().empty() ? "(no shader)" : pipeline.Shaders().front().substr(0, 22).c_str());
    }
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
        "its shaders read constants from their own code, and AnyBox cannot give them a code base",
    };
    static std::atomic<std::uint64_t> skipped[Count]{};
    Reason reason = Count;
    VkPipeline variant = VK_NULL_HANDLE;
    std::string variantWhy;
    const int formatChoice = pipeline_->BindPoint() == VK_PIPELINE_BIND_POINT_COMPUTE && pipeline_->FormatSpecialized() ? ChooseFormatVariant(function, variant, variantWhy) : 0;
    const auto checkCodeBase = [&] {
        std::string why;
        if (!ResolveCodeBase(function, why)) {
            reason = CodeBase;
            static std::mutex whyLock;
            static std::set<std::string> told;
            const std::lock_guard guard(whyLock);
            if (told.size() < 64 && told.insert(why).second) AbxLog("d3d12_x", "DIAG no code base for %s: %s", pipeline_->Shaders().empty() ? "(no shader)" : pipeline_->Shaders().front().c_str(), why.c_str());
        }
    };
    if (formatChoice == 2) {
        reason = Pending;
    } else if (formatChoice == 3) {
        reason = Unbuilt;
    } else if (formatChoice == 1) {
        if (pipeline_->UsesCodeBase()) checkCodeBase();
    } else if (pipeline_->Pending() && pipeline_->BindPoint() == VK_PIPELINE_BIND_POINT_COMPUTE) {
        static std::atomic<std::uint64_t> waited{0};
        const std::uint64_t count = ++waited;
        if (count == 1 || (count & (count - 1)) == 0) {
            AbxLog("d3d12_x", "compute work whose pipeline is still being built waits for that build instead of being skipped, as titles can depend on its results (%llu so far)",
                   static_cast<unsigned long long>(count));
        }
        pipeline_->BuildNow();
    } else if (pipeline_->Pending() && DrawsWaitForPipelines()) {
        static std::atomic<std::uint64_t> waited{0};
        const std::uint64_t count = ++waited;
        if (count == 1 || (count & (count - 1)) == 0) {
            AbxLog("d3d12_x", "a draw whose pipeline is still being built waits for that build instead of being skipped, as a title that draws something once (a lookup "
                              "table it renders while it starts) never gets it otherwise (%llu so far; d3d12_x.draws_wait_for_pipelines = 0 skips such draws again)",
                   static_cast<unsigned long long>(count));
        }
        pipeline_->BuildNow();
    }
    if (formatChoice != 0) {
    } else if (pipeline_->Pending()) {
        reason = Pending;
        pipeline_->Prioritize();
    }
    else if (pipeline_->Pipeline() == VK_NULL_HANDLE) reason = Unbuilt;
    else if (pipeline_->UsesCodeBase()) checkCodeBase();
    const auto unbuiltText = [&] { return formatChoice == 3 ? variantWhy : pipeline_->UnbuiltReason(); };
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
        if (checkpointDetail_ != nullptr && markers.size() < 200000) name += checkpointDetail_;
        Recorder(function).Checkpoint(markers.insert(std::string(function) + " " + name).first->c_str());
    }
    if (TracingFrame()) {
        for (Resource* target : targetResources_) NoteTracedTarget(target);
        if (depthStencil_) NoteTracedDepth(depthResource_);
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
        const unsigned list = static_cast<unsigned>((reinterpret_cast<std::uintptr_t>(this) >> 4) & 0xFFFFu);
        const bool computeTraced = pipeline_->BindPoint() == VK_PIPELINE_BIND_POINT_COMPUTE;
        std::string everyShader;
        for (const std::string& shader : pipeline_->Shaders()) everyShader += shader + " ";
        const bool watched = WatchedShader(everyShader);
        AbxLog("d3d12_x", "TRACE %s%s%s | targets%s%s%s | list %04x", function, shaders.c_str(), reason == Count ? "" : (" SKIPPED: " + unbuiltText()).c_str(),
               targets.c_str(), depthStencil_ ? " +depth" : "", rectangles_ ? " rects" : "", list);
        const RootArguments& arguments = computeTraced ? compute_ : graphics_;
        {
            const std::uint32_t rootWords = arguments.signature != nullptr ? arguments.signature->Layout().words : 0;
            std::string user;
            for (std::uint32_t word = 0; word < rootWords && word < arguments.words.size(); ++word) {
                char piece[16];
                std::snprintf(piece, sizeof(piece), " %08x", arguments.words[word]);
                user += piece;
            }
            AbxLog("d3d12_x", "TRACE     user data, %u root words from s%u:%s; heap indexing flags 0x%x, resource heap 0x%llx, sampler heap 0x%llx | list %04x", rootWords,
                   computeTraced ? kComputeFirstRootSgpr : kGraphicsFirstRootSgpr, user.c_str(), HeapIndexing(arguments.signature),
                   static_cast<unsigned long long>(shaderResourceHeap_ != nullptr ? shaderResourceHeap_->GpuStart().ptr : 0),
                   static_cast<unsigned long long>(samplerHeap_ != nullptr ? samplerHeap_->GpuStart().ptr : 0), list);
            if (!pipeline_->Shaders().empty() && (computeTraced || watched)) {
                DumpTracedBuffers(context_.addresses, computeTraced ? pipeline_->Shaders().front() : everyShader, list, arguments.words.data(),
                                  (std::min)(static_cast<std::size_t>(rootWords), arguments.words.size()));
            }
        }
        {
            UINT increment = 0;
            const std::uint8_t* table = DescriptorHeap::AtGpuAddress(arguments.words[0], increment);
            if (table != nullptr) {
                std::uint32_t slots[16] = {};
                std::memcpy(slots, table, sizeof(slots));
                AbxLog("d3d12_x", "TRACE     table 0x%x at record: %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | list %04x", arguments.words[0],
                       slots[0], slots[1], slots[2], slots[3], slots[4], slots[5], slots[6], slots[7], slots[8], slots[9], slots[10], slots[11], slots[12], slots[13], slots[14],
                       slots[15], list);
                NoteTracedTable(arguments.words[0]);
                const std::uint64_t base = (static_cast<std::uint64_t>(slots[0]) | (static_cast<std::uint64_t>(slots[1] & 0xFFFF) << 32));
                if (context_.addresses != nullptr && base != 0) {
                    const std::uint64_t recordBytes = static_cast<std::uint64_t>((std::max)(slots[1] >> 16, 1u)) * slots[2];
                    AbxLog("d3d12_x", "TRACE     V# base 0x%llx (%u bytes) is %s the GPU page table: %s%s | list %04x", static_cast<unsigned long long>(base), slots[2],
                           context_.addresses->Contains(base, slots[2]) ? "in" : "NOT in", context_.addresses->DescribeTitleAddress(base).c_str(),
                           TitleWordsText(context_.addresses, base, recordBytes).c_str(), list);
                }
            }
        }
        for (std::size_t word = 0; word < arguments.words.size(); ++word) {
            UINT increment = 0;
            const std::uint8_t* table = DescriptorHeap::AtGpuAddress(arguments.words[word], increment);
            if (table == nullptr || increment < 32) continue;
            const UINT slotLimit = watched ? 192u : 12u;
            for (UINT slot = 0; slot < slotLimit; ++slot) {
                UINT slotIncrement = 0;
                const std::uint8_t* at = DescriptorHeap::AtGpuAddress(static_cast<std::uint64_t>(arguments.words[word]) + static_cast<std::uint64_t>(slot) * increment, slotIncrement);
                if (at == nullptr) break;
                std::uint32_t descriptor[8] = {};
                std::memcpy(descriptor, at, sizeof(descriptor));
                if ((descriptor[3] >> 28) < 8) {
                    const std::uint64_t base = static_cast<std::uint64_t>(descriptor[0]) | (static_cast<std::uint64_t>(descriptor[1] & 0xFFFFu) << 32);
                    const std::uint32_t stride = (descriptor[1] >> 16) & 0x3FFFu;
                    const std::uint64_t bytes = stride != 0 ? static_cast<std::uint64_t>(stride) * descriptor[2] : descriptor[2];
                    if (computeTraced && base != 0 && bytes != 0 && context_.addresses != nullptr) {
                        AbxLog("d3d12_x", "TRACE     root word %zu slot %u: buffer at title 0x%llx, %llu bytes (stride %u, %u records); %s | list %04x", word, slot,
                               static_cast<unsigned long long>(base), static_cast<unsigned long long>(bytes), stride, descriptor[2],
                               (context_.addresses->DescribeBacking(base, bytes) + "; " + context_.addresses->DescribePlacement(base, bytes)).c_str(), list);
                    }
                    continue;
                }
                Resource* read = nullptr;
                int readFormat = 0;
                const std::string described = DescriptorWriter::DescribeImageWords(descriptor, &read, &readFormat);
                AbxLog("d3d12_x", "TRACE     root word %zu slot %u: %s | list %04x", word, slot, described.c_str(), list);
                if (computeTraced && read != nullptr && (readFormat == static_cast<int>(DXGI_FORMAT_R32_FLOAT) || readFormat == static_cast<int>(DXGI_FORMAT_R32_TYPELESS))) {
                    NoteTracedInput(read);
                } else if (read != nullptr) {
                    NoteTracedRead(read);
                }
                if (watched && read != nullptr) NoteWatchedRead(read);
            }
        }
    }
    if (reason == Count) {
        ++(pipeline_->BindPoint() == VK_PIPELINE_BIND_POINT_COMPUTE ? Stats().dispatches : Stats().draws);
        if (profileGeneration_ != 0) GpuProfile::MarkWork(commands_, profileGeneration_, profileList_, function, pipeline_->Shaders());
        if (variant != VK_NULL_HANDLE) {
            Recorder(function).BindPipeline(pipeline_->BindPoint(), variant);
            pipelineBound_ = false;
            workPipeline_ = variant;
        } else {
            if (!pipelineBound_) {
                Recorder(function).BindPipeline(pipeline_->BindPoint(), pipeline_->Pipeline());
                pipelineBound_ = true;
            }
            workPipeline_ = pipeline_->Pipeline();
        }
        return false;
    }
    if (pipeline_->BindPoint() == VK_PIPELINE_BIND_POINT_COMPUTE) NoteComputeSkip(*pipeline_, reason == Pending ? kReasons[Pending] : unbuiltText().empty() ? kReasons[reason] : unbuiltText().c_str());
    ++(reason == Pending ? Stats().skippedBuilding : Stats().skippedOther);
    const std::uint64_t total = ++skipped[reason];
    bool power = total == 1;
    for (std::uint64_t step = 10; step <= total && !power; step *= 10) power = total == step;
    if (power) {
        AbxLog("d3d12_x", "%s skipped (%llu so far for this reason): %s%s%s", function, static_cast<unsigned long long>(total), kReasons[reason],
               reason == Unbuilt ? ": " : "", reason == Unbuilt ? unbuiltText().c_str() : "");
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
    if (depthStencil_ && depthResource_ != nullptr) {
        if (Resource::DepthShadow* const shadow = depthResource_->DepthShadowOf()) {
            const GraphicsDescription* const description = pipeline_->Description();
            const bool readOnly = (static_cast<UINT>(depthStencil_->readOnly) & D3D12_DSV_FLAG_READ_ONLY_DEPTH) != 0;
            const bool writes = description == nullptr || (description->desc.DepthStencilState.DepthEnable != FALSE &&
                                                           description->desc.DepthStencilState.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL);
            if (!readOnly && writes) shadow->planeNewer.store(true, std::memory_order_relaxed);
        }
    }
    PushUserData(graphics_, kGraphicsFirstRootSgpr, function);
    BindTitleSet(VK_PIPELINE_BIND_POINT_GRAPHICS);
    if (checks_ != VK_NULL_HANDLE && pipeline_->MayWriteMemory()) checksWritten_ = true;
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
    for (Resource* const target : targetResources_) {
        if (target != nullptr && target->ReadBack() && std::find(readbackWritten_.begin(), readbackWritten_.end(), target) == readbackWritten_.end()) {
            readbackWritten_.push_back(target);
        }
    }
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
    ours->NoteBound();
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
    if (type == D3D12_ROOT_PARAMETER_TYPE_UAV && address != 0 && context_.addresses != nullptr) context_.addresses->MarkGpuRootWrite(address);
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
    const TimedScope timed(TimedPart::ListClose);
    EndPass();
    std::vector<Resource*> closing;
    closing.swap(readbackWritten_);
    for (const auto& [key, state] : states_) {
        if (state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS || (key.second != kAllSubresources && key.second != 0) || key.first->IsBuffer() || !key.first->ReadBack()) continue;
        Resource* const texture = const_cast<Resource*>(key.first);
        if (std::find(closing.begin(), closing.end(), texture) == closing.end()) closing.push_back(texture);
    }
    for (Resource* const target : closing) {
        RecordReadback(target, StateOf(target, 0, D3D12_RESOURCE_STATE_RENDER_TARGET), "ID3D12GraphicsCommandList::Close");
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
    if (profileGeneration_ != 0) GpuProfile::EndList(commands_, profileGeneration_, profileList_);
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
    const TitleAddressSpace::Naming naming(function);
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
    const TimedScope timed(TimedPart::ListWork);
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
    const TimedScope timed(TimedPart::ListWork);
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
    const TimedScope timed(TimedPart::ListWork);
    char groups[64];
    std::snprintf(groups, sizeof(groups), ", %u x %u x %u groups", x, y, z);
    checkpointDetail_ = groups;
    const bool prepared = PrepareDraw(function);
    checkpointDetail_ = nullptr;
    if (!prepared) return;
    const std::uint32_t* limits = context_.device.MeshMaxGroups();
    const std::uint64_t total = std::uint64_t{x} * y * z;
    const auto shaders = [this] {
        std::string names;
        for (const std::string& shader : pipeline_->Shaders()) names += (names.empty() ? "" : " ") + shader.substr(0, 20);
        return names;
    };
    static std::atomic<std::uint64_t> largest{0};
    std::uint64_t seen = largest.load();
    while (total > seen && total >= 2 * seen) {
        if (!largest.compare_exchange_weak(seen, total)) continue;
        AbxLog("d3d12_x", "DIAG the largest direct mesh dispatch so far: %u x %u x %u = %llu groups, %s (logged each time the largest at least doubles)", x, y, z,
               static_cast<unsigned long long>(total), shaders().c_str());
        break;
    }
    static const std::uint64_t groupsMax = [] {
        const char* value = AbxSetting("d3d12_x.mesh_groups_max");
        return value != nullptr ? std::strtoull(value, nullptr, 10) : 0ull;
    }();
    if (groupsMax != 0 && total > groupsMax) {
        ++Stats().skippedOther;
        static std::atomic<std::uint64_t> capped{0};
        const std::uint64_t count = ++capped;
        if (count == 1 || (count & (count - 1)) == 0) {
            AbxLog("d3d12_x", "a mesh dispatch of %u x %u x %u = %llu groups is past d3d12_x.mesh_groups_max (%llu) and is skipped: %s (%llu so far)", x, y, z,
                   static_cast<unsigned long long>(total), static_cast<unsigned long long>(groupsMax), shaders().c_str(), static_cast<unsigned long long>(count));
        }
        return;
    }
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
    if (TracingFrame()) AbxLog("d3d12_x", "TRACE   DispatchMesh(%u x %u x %u groups) %s", x, y, z, shaders().c_str());
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "mesh dispatches run as Vulkan mesh shader draws, with the mesh stage's root words moved from s16 to s8 where the pixel stage reads them (first: %u x %u x %u)", x,
               y, z);
        ReportRootState(graphics_, "first DispatchMesh");
    }
}

void STDMETHODCALLTYPE CommandList::Dispatch(UINT x, UINT y, UINT z) {
    const char* function = "ID3D12GraphicsCommandList::Dispatch";
    const TimedScope timed(TimedPart::ListWork);
    EndPass();
    if (pipeline_ == nullptr || pipeline_->BindPoint() != VK_PIPELINE_BIND_POINT_COMPUTE) {
        AbxUnsupported(function, pipeline_ == nullptr ? "a dispatch with no pipeline state set on this command list since its last reset"
                                                      : ("a dispatch while a graphics pipeline state is set: " + std::string(pipeline_->Shaders().empty() ? pipeline_->UnbuiltReason() : pipeline_->Shaders().front())).c_str());
    }
    if (SkippedWork(function)) return;
    if (TracingFrame()) {
        AbxLog("d3d12_x", "TRACE     Dispatch(%u x %u x %u groups) %s | list %04x", x, y, z, pipeline_->Shaders().empty() ? "" : pipeline_->Shaders().front().substr(0, 19).c_str(),
               static_cast<unsigned>((reinterpret_cast<std::uintptr_t>(this) >> 4) & 0xFFFFu));
    }
    PushUserData(compute_, kComputeFirstRootSgpr, function);
    BindTitleSet(VK_PIPELINE_BIND_POINT_COMPUTE);
    Recorder(function).Dispatch(x, y, z);
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) ReportRootState(compute_, "first Dispatch");
}

void CommandList::DispatchThreads(UINT x, UINT y, UINT z, UINT flags) {
    const char* function = "D3D12 command list slot 80 (DispatchX)";
    if ((flags & kDispatchCountsThreads) == 0) {
        Dispatch(x, y, z);
        return;
    }
    const TimedScope timed(TimedPart::ListWork);
    EndPass();
    if (pipeline_ == nullptr || pipeline_->BindPoint() != VK_PIPELINE_BIND_POINT_COMPUTE) {
        AbxUnsupported(function, pipeline_ == nullptr ? "a dispatch with no pipeline state set on this command list since its last reset" : "a dispatch while a graphics pipeline state is set");
    }
    if (!pipeline_->FormatSpecialized() && SkippedWork(function)) return;
    std::array<std::uint32_t, 3> size{};
    std::string error;
    if (!pipeline_->GroupSize(size, error)) {
        AbxUnsupported(function, ("a dispatch that counts threads for a compute shader whose thread group size is not known: " + error).c_str());
    }
    if (y % size[1] != 0 || z % size[2] != 0) {
        AbxUnsupported(function, "a dispatch that counts threads and ends in a partial thread group in y or z; only a partial last group in x is limited");
    }
    const UINT groups = x / size[0] + (x % size[0] != 0 ? 1u : 0u);
    const bool partial = x % size[0] != 0;
    VkPipeline limited = VK_NULL_HANDLE;
    if (pipeline_->FormatSpecialized() && !partial && SkippedWork(function)) return;
    if (partial && pipeline_->FormatSpecialized()) {
        ++Stats().skippedOther;
        static std::atomic<std::uint64_t> told{0};
        const std::uint64_t count = ++told;
        if (count == 1 || (count & (count - 1)) == 0) {
            AbxLog("d3d12_x", "a dispatch that counts threads and ends in a partial thread group is skipped: its pipeline %s is built for the buffer formats its dispatch reads, "
                              "and no thread-limited form of such a pipeline is built (%llu so far)",
                   pipeline_->Shaders().empty() ? "(no shader)" : pipeline_->Shaders().front().c_str(), static_cast<unsigned long long>(count));
        }
        return;
    }
    if (partial) {
        limited = pipeline_->ThreadLimited(kThreadLimitSgpr, error);
        if (limited == VK_NULL_HANDLE) {
            AbxUnsupported(function, ("a dispatch that counts threads and ends in a partial thread group, whose thread-limited pipeline was not built: " + error).c_str());
        }
    }
    static std::atomic<std::uint32_t> reported{0};
    if (reported.fetch_add(1) < 16) {
        AbxLog("d3d12_x", "DispatchX with flags 0x%x counts threads: %u x %u x %u threads of %s (thread group %u x %u x %u) run as %u x %u x %u groups%s", flags, x, y, z,
               pipeline_->Shaders().empty() ? "(no shader)" : pipeline_->Shaders().front().substr(0, 19).c_str(), size[0], size[1], size[2], groups, y / size[1], z / size[2],
               partial ? ", the threads past the count returning at once" : "");
    }
    if (TracingFrame()) {
        AbxLog("d3d12_x", "TRACE     DispatchX(%u x %u x %u threads, flags 0x%x) as %u x %u x %u groups %s", x, y, z, flags, groups, y / size[1], z / size[2],
               pipeline_->Shaders().empty() ? "" : pipeline_->Shaders().front().substr(0, 19).c_str());
    }
    PushUserData(compute_, kComputeFirstRootSgpr, function, partial ? x : 0);
    BindTitleSet(VK_PIPELINE_BIND_POINT_COMPUTE);
    if (partial) Recorder(function).BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, limited);
    Recorder(function).Dispatch(groups, y / size[1], z / size[2]);
    if (partial) pipelineBound_ = false;
}

void CommandList::CopyMemoryToMemory(std::uint64_t destination, std::uint64_t source, std::uint64_t bytes, const char* function) {
    const TimedScope timed(TimedPart::ListCopies);
    if (context_.addresses == nullptr) AbxUnsupported(function, "a memory copy on a device without a title address space");
    if (bytes == 0) return;
    if (destination < source + bytes && source < destination + bytes) AbxUnsupported(function, "a memory copy whose source and destination overlap");
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "CopyMemoryToMemoryX copies between title addresses on the GPU (first: 0x%llx -> 0x%llx, %llu bytes)", static_cast<unsigned long long>(source),
               static_cast<unsigned long long>(destination), static_cast<unsigned long long>(bytes));
    }
    const TitleAddressSpace::Naming naming(function);
    const bool throughTable = CopyThroughTable(destination, source, bytes, function);
    if (TracingFrame()) {
        AbxLog("d3d12_x", "TRACE %s: %llu bytes from title 0x%llx to title 0x%llx, %s | list %04x", function, static_cast<unsigned long long>(bytes),
               static_cast<unsigned long long>(source), static_cast<unsigned long long>(destination),
               throughTable ? "through the page table" : "naming the memory as buffers", static_cast<unsigned>((reinterpret_cast<std::uintptr_t>(this) >> 4) & 0xFFFFu));
    }
    if (throughTable) return;
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

bool CommandList::CopyThroughTable(std::uint64_t destination, std::uint64_t source, std::uint64_t bytes, const char* function) {
    if (context_.addresses == nullptr || !context_.addresses->TableCommands()) return false;
    const TableCopy* const table = TableCopy::For(context_.device);
    if (table == nullptr) return false;
    const TitleAccess accesses[2] = {{destination, bytes, true}, {source, bytes, false}};
    if (!context_.addresses->TableCommand(accesses, 2)) return false;
    OrderCopy(function);
    const std::uint64_t root = context_.addresses->RootTable();
    for (std::uint64_t done = 0; done < bytes; done += kTableCopyBytes) {
        table->Copy(commands_, root, destination + done, false, source + done, false, (std::min)(bytes - done, kTableCopyBytes));
    }
    pipelineBound_ = false;
    computeSetBound_ = false;
    return true;
}

bool CommandList::FillThroughTable(std::uint64_t destination, std::uint64_t bytes, std::uint32_t value, const char* function) {
    if (context_.addresses == nullptr || !context_.addresses->TableCommands()) return false;
    const TableCopy* const table = TableCopy::For(context_.device);
    if (table == nullptr) return false;
    const TitleAccess access{destination, bytes, true};
    if (!context_.addresses->TableCommand(&access, 1)) return false;
    OrderCopy(function);
    const std::uint64_t root = context_.addresses->RootTable();
    for (std::uint64_t done = 0; done < bytes; done += kTableCopyBytes) {
        table->Fill(commands_, root, destination + done, (std::min)(bytes - done, kTableCopyBytes), value);
    }
    pipelineBound_ = false;
    computeSetBound_ = false;
    return true;
}

std::optional<TitleAddressSpace::Location> CommandList::LocateTitleMemory(std::uint64_t address, std::uint64_t wanted, bool write, std::uint64_t& available, std::uint64_t bytes,
                                                                         const char* function) {
    std::optional<TitleAddressSpace::Location> found = context_.addresses->LocateRun(address, available, wanted);
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
    found = context_.addresses->LocateRun(address, available, wanted);
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
    const TitleAddressSpace::Naming naming(function);
    const bool throughTable = FillThroughTable(destination, bytes, value, function);
    if (TracingFrame()) {
        AbxLog("d3d12_x", "TRACE %s: %llu bytes at title 0x%llx filled with 0x%08x, %s | list %04x", function, static_cast<unsigned long long>(bytes),
               static_cast<unsigned long long>(destination), value, throughTable ? "through the page table" : "naming the memory as a buffer",
               static_cast<unsigned>((reinterpret_cast<std::uintptr_t>(this) >> 4) & 0xFFFFu));
    }
    if (throughTable) return;
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
    const TimedScope timed(TimedPart::ListCopies);
    const char* function = "ID3D12GraphicsCommandList::CopyBufferRegion";
    Resource* to = Ours(destination, function);
    Resource* from = Ours(source, function);
    if (!to->IsBuffer() || !from->IsBuffer()) {
        AbxUnsupported(function, "a copy between buffers with a texture");
    }
    const TitleAddressSpace::Naming naming(function);
    const bool inside = destinationOffset <= to->Width() && bytes <= to->Width() - destinationOffset && sourceOffset <= from->Width() && bytes <= from->Width() - sourceOffset;
    const std::uint64_t destinationTitle = to->GetGPUVirtualAddress() + destinationOffset;
    const std::uint64_t sourceTitle = from->GetGPUVirtualAddress() + sourceOffset;
    const bool throughTable = inside && bytes != 0 && CopyThroughTable(destinationTitle, sourceTitle, bytes, function);
    if (TracingFrame()) {
        AbxLog("d3d12_x", "TRACE %s: %llu bytes from title 0x%llx to title 0x%llx, %s | list %04x", function, static_cast<unsigned long long>(bytes),
               static_cast<unsigned long long>(sourceTitle), static_cast<unsigned long long>(destinationTitle),
               throughTable ? "through the page table" : "naming the memory as buffers", static_cast<unsigned>((reinterpret_cast<std::uintptr_t>(this) >> 4) & 0xFFFFu));
    }
    if (throughTable) return;
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
    to->NoteListUse(true);
    from->NoteListUse(false);
    if (toTexture && !to->IsBuffer()) {
        if (Resource::DepthShadow* const shadow = to->DepthShadowOf()) shadow->planeNewer.store(true, std::memory_order_relaxed);
    }
    if (toTexture && !to->IsBuffer() && to->Texture().desc.Width >= 1024) {
        static std::atomic<int> diag{0};
        if (diag++ < 30) {
            AbxLog("d3d12_x", "DIAG copy into texture %p (%llux%u f%d) sub %u from %s %p footprint f%d %ux%u rowpitch %u offset %llu", static_cast<void*>(to->Texture().image),
                   static_cast<unsigned long long>(to->Texture().desc.Width), to->Texture().desc.Height, static_cast<int>(to->Texture().desc.Format), destination->SubresourceIndex,
                   fromTexture ? "texture" : "buffer", static_cast<void*>(from), static_cast<int>(source->PlacedFootprint.Footprint.Format), source->PlacedFootprint.Footprint.Width,
                   source->PlacedFootprint.Footprint.Height, source->PlacedFootprint.Footprint.RowPitch, static_cast<unsigned long long>(source->PlacedFootprint.Offset));
        }
    }
    if (TracingFrame() && toTexture && !to->IsBuffer()) {
        const D3D12_RESOURCE_DESC& into = to->Texture().desc;
        char origin[96];
        if (fromTexture && !from->IsBuffer()) {
            std::snprintf(origin, sizeof(origin), "texture %p subresource %u", static_cast<void*>(from->Texture().image), source->SubresourceIndex);
        } else {
            std::snprintf(origin, sizeof(origin), "a buffer at title 0x%llx + %llu", static_cast<unsigned long long>(from->TitleAddress()),
                          static_cast<unsigned long long>(source->PlacedFootprint.Offset));
        }
        AbxLog("d3d12_x", "TRACE %s into texture %p %llux%u f%d subresource %u from %s", function, static_cast<void*>(to->Texture().image), static_cast<unsigned long long>(into.Width),
               into.Height, static_cast<int>(into.Format), destination->SubresourceIndex, origin);
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
    const TimedScope timed(TimedPart::ListCopies);
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
    if (Resource::DepthShadow* const shadow = to->DepthShadowOf()) shadow->planeNewer.store(true, std::memory_order_relaxed);
    to->NoteListUse(true);
    from->NoteListUse(false);
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
    topology_ = topology;
}

void STDMETHODCALLTYPE CommandList::RSSetViewports(UINT count, const D3D12_VIEWPORT* viewports) {
    const char* function = "ID3D12GraphicsCommandList::RSSetViewports";
    if (count != 0 && viewports == nullptr) {
        AbxUnsupported(function, "viewports without a viewport array");
    }
    if (count != 0 && TracingFrame()) {
        AbxLog("d3d12_x", "TRACE     viewport at %g %g, %g x %g, depth %g..%g (%u in all) | list %04x", static_cast<double>(viewports[0].TopLeftX),
               static_cast<double>(viewports[0].TopLeftY), static_cast<double>(viewports[0].Width), static_cast<double>(viewports[0].Height),
               static_cast<double>(viewports[0].MinDepth), static_cast<double>(viewports[0].MaxDepth), count,
               static_cast<unsigned>((reinterpret_cast<std::uintptr_t>(this) >> 4) & 0xFFFFu));
    }
    if (count == 0) {
        Recorder(function);
        return;
    }
    std::string error;
    const std::vector<D3D12_VIEWPORT> list = count != 0 ? std::vector<D3D12_VIEWPORT>(viewports, viewports + count) : std::vector<D3D12_VIEWPORT>{};
    Check(Recorder(function).SetViewports(list, error), function, error);
    viewports_ = list;
}

void STDMETHODCALLTYPE CommandList::RSSetScissorRects(UINT count, const D3D12_RECT* rects) {
    const char* function = "ID3D12GraphicsCommandList::RSSetScissorRects";
    if (count != 0 && rects == nullptr) {
        AbxUnsupported(function, "scissor rectangles without a rectangle array");
    }
    if (count != 0 && TracingFrame()) {
        AbxLog("d3d12_x", "TRACE     scissor %ld %ld to %ld %ld (%u in all) | list %04x", static_cast<long>(rects[0].left), static_cast<long>(rects[0].top),
               static_cast<long>(rects[0].right), static_cast<long>(rects[0].bottom), count, static_cast<unsigned>((reinterpret_cast<std::uintptr_t>(this) >> 4) & 0xFFFFu));
    }
    if (count == 0) {
        Recorder(function);
        return;
    }
    std::string error;
    const std::vector<D3D12_RECT> list = count != 0 ? std::vector<D3D12_RECT>(rects, rects + count) : std::vector<D3D12_RECT>{};
    Check(Recorder(function).SetScissors(list, error), function, error);
    scissors_ = list;
}

void STDMETHODCALLTYPE CommandList::OMSetBlendFactor(const FLOAT factor[4]) {
    const float ones[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const float* const used = factor != nullptr ? factor : ones;
    Recorder("ID3D12GraphicsCommandList::OMSetBlendFactor").SetBlendFactor(used);
    std::copy(used, used + 4, blendFactor_.begin());
}

void STDMETHODCALLTYPE CommandList::OMSetStencilRef(UINT reference) {
    if (TracingFrame()) {
        AbxLog("d3d12_x", "TRACE     stencil reference 0x%x | list %04x", reference, static_cast<unsigned>((reinterpret_cast<std::uintptr_t>(this) >> 4) & 0xFFFFu));
    }
    Recorder("ID3D12GraphicsCommandList::OMSetStencilRef").SetStencilReference(reference);
    stencilReference_ = reference;
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
    const TimedScope timed(TimedPart::ListBarriers);
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
            constexpr UINT kWritableStates = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_DEPTH_WRITE |
                                             D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST;
            resource->NoteListUse((static_cast<UINT>(barrier.Transition.StateAfter) & kWritableStates) != 0);
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
            const auto first = states_.lower_bound({resource, 0});
            auto last = first;
            while (last != states_.end() && last->first.first == resource) ++last;
            states_.erase(first, last);
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
    for (UINT index = 0; index < count; ++index) {
        const D3D12_RESOURCE_BARRIER& barrier = barriers[index];
        if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
        Resource* const resource = Ours(barrier.Transition.pResource, function);
        if (resource->IsBuffer() || resource->DepthShadowOf() == nullptr) continue;
        if (barrier.Transition.Subresource != kAllSubresources && barrier.Transition.Subresource != 0) continue;
        const bool before = (static_cast<UINT>(barrier.Transition.StateBefore) & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0;
        const bool after = (static_cast<UINT>(barrier.Transition.StateAfter) & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0;
        if (before != after) RecordDepthShadow(resource, barrier.Transition.StateAfter, after, function);
    }
    for (UINT index = 0; index < count; ++index) {
        const D3D12_RESOURCE_BARRIER& barrier = barriers[index];
        if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
        Resource* const resource = Ours(barrier.Transition.pResource, function);
        if (resource->IsBuffer() || !resource->ReadBack()) continue;
        if (barrier.Transition.Subresource != kAllSubresources && barrier.Transition.Subresource != 0) continue;
        constexpr UINT kWriteStates = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        const auto drawn = std::find(readbackWritten_.begin(), readbackWritten_.end(), resource);
        if (drawn == readbackWritten_.end() && (static_cast<UINT>(barrier.Transition.StateBefore) & kWriteStates) == 0) continue;
        if (drawn != readbackWritten_.end()) readbackWritten_.erase(drawn);
        RecordReadback(resource, barrier.Transition.StateAfter, function);
    }
}

void CommandList::RecordDepthShadow(Resource* resource, D3D12_RESOURCE_STATES state, bool enters, const char* function) {
    Resource::DepthShadow* const shadow = resource->DepthShadowOf();
    if (shadow == nullptr) return;
    if ((Resource::DepthUnorderedAccess() & 4ul) != 0) return;
    if (enters && !shadow->planeNewer.exchange(false)) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            AbxLog("d3d12_x", "%s: the depth plane of the texture at title 0x%llx enters the unordered access state as its unordered access image left it (no draw, clear or copy "
                              "wrote the plane since), so nothing is copied; later such barriers are not logged",
                   function, static_cast<unsigned long long>(resource->TextureAddress()));
        }
        return;
    }
    EndPass();
    const DeviceFunctions& vk = context_.device.Vk();
    CommandRecorder recorder(context_.device, commands_);
    const D3D12_RESOURCE_STATES workState = enters ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_DEPTH_WRITE;
    std::string error;
    static std::atomic<std::uint64_t> recorded{0};
    const std::uint64_t number = ++recorded;
    if (state != workState && !recorder.Transition(resource->Texture(), state, workState, error)) {
        static std::atomic<int> reported{0};
        if (reported.fetch_add(1) < 8) {
            AbxLog("d3d12_x", "%s: the depth plane of the texture at title 0x%llx was not brought in step with its unordered access image (state 0x%x): %s", function,
                   static_cast<unsigned long long>(resource->TextureAddress()), static_cast<unsigned>(state), error.c_str());
        }
        if (enters) shadow->planeNewer.store(true, std::memory_order_relaxed);
        return;
    }
    VkMemoryBarrier2 all{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    all.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    all.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    all.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    all.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo between{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    between.memoryBarrierCount = 1;
    between.pMemoryBarriers = &all;
    vk.cmdPipelineBarrier2(commands_, &between);
    if (context_.device.Checkpoints()) {
        recorder.Checkpoint(enters ? "ResourceBarrier: a depth plane copied into its unordered access image" : "ResourceBarrier: a depth plane drawn from its unordered access image");
    }
    if (enters) {
        VkBufferImageCopy color{};
        color.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        color.imageExtent = shadow->extent;
        VkBufferImageCopy depth = color;
        depth.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vk.cmdCopyImageToBuffer(commands_, resource->Texture().image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shadow->staging.buffer, 1, &depth);
        vk.cmdPipelineBarrier2(commands_, &between);
        vk.cmdCopyBufferToImage(commands_, shadow->staging.buffer, shadow->image, VK_IMAGE_LAYOUT_GENERAL, 1, &color);
    } else {
        shadow->pass->Record(commands_, shadow->pipeline, shadow->source, shadow->plane, shadow->stencil, {shadow->extent.width, shadow->extent.height},
                             type_ != D3D12_COMMAND_LIST_TYPE_DIRECT);
        shadow->planeNewer.store(false, std::memory_order_relaxed);
        pipelineBound_ = false;
        graphicsSetBound_ = false;
        if (!viewports_.empty()) Check(recorder.SetViewports(viewports_, error), function, error);
        if (!scissors_.empty()) Check(recorder.SetScissors(scissors_, error), function, error);
        if (topology_) Check(recorder.SetPrimitiveTopology(*topology_, false, error), function, error);
    }
    vk.cmdPipelineBarrier2(commands_, &between);
    if (state != workState && !recorder.Transition(resource->Texture(), workState, state, error)) {
        AbxUnsupported(function, ("a depth texture could not be returned to its state after its unordered access image was brought in step: " + error).c_str());
    }
    copied_ = false;
    if (number <= 4 || (number & (number - 1)) == 0) {
        AbxLog("d3d12_x", "%s: the depth plane of the texture at title 0x%llx %s its unordered access image (%ux%u, state after 0x%x, %llu such steps so far)", function,
               static_cast<unsigned long long>(resource->TextureAddress()), enters ? "was copied into" : "was drawn from", shadow->extent.width, shadow->extent.height,
               static_cast<unsigned>(state), static_cast<unsigned long long>(number));
    }
}

void CommandList::RecordReadback(Resource* resource, D3D12_RESOURCE_STATES state, const char* function) {
    TextureReadback* const readback = TextureReadback::Active();
    if (readback == nullptr) return;
    EndPass();
    std::string error;
    if (readback->Record(*resource, state, commands_, error)) return;
    static std::atomic<int> reported{0};
    if (reported.fetch_add(1) < 8) {
        const D3D12_RESOURCE_DESC& desc = resource->Texture().desc;
        AbxLog("d3d12_x", "%s: the copy of a small GPU-written texture (%llux%u, format %d, title address 0x%llx, state 0x%x) back to title memory was not recorded: %s", function,
               static_cast<unsigned long long>(desc.Width), desc.Height, static_cast<int>(desc.Format), static_cast<unsigned long long>(resource->TextureAddress()),
               static_cast<unsigned>(state), error.c_str());
    }
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
    const BufferRef range = WindowRange(view->BufferLocation, view->SizeInBytes, function);
    Check(Recorder(function).SetIndexBuffer(range, view->Format, error), function, error);
    indexBuffer_ = std::make_pair(range, view->Format);
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
        if (start + index < vertexBuffers_.size()) vertexBuffers_[start + index] = VertexBufferRef{range, views[index].StrideInBytes};
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
    Resource* depthResource = nullptr;
    if (depthStencil != nullptr) {
        const DescriptorRecord& record = Attachment(*depthStencil, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, function);
        if (record.kind == HeapEntry::DepthStencil) {
            depth = record.attachment;
            depthResource = record.resource;
        }
    }
    targetsSet_ = true;
    emptyTargets_ = 0;
    for (const AttachmentRef& color : colors) emptyTargets_ += color.view == VK_NULL_HANDLE ? 1u : 0u;
    depthHandleGiven_ = depthStencil != nullptr;
    renderTargets_ = std::move(colors);
    targetResources_ = std::move(targetResources);
    depthStencil_ = depth;
    depthResource_ = depthResource;
    for (Resource* const target : targetResources_) {
        if (target != nullptr) target->NoteListUse(true);
    }
    if (depthResource_ != nullptr) depthResource_->NoteListUse(true);
}

void STDMETHODCALLTYPE CommandList::ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE view, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil, UINT count, const D3D12_RECT* rects) {
    const TimedScope timed(TimedPart::ListCopies);
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
    if (record.resource != nullptr) record.resource->NoteListUse(true);
    if ((static_cast<UINT>(flags) & D3D12_CLEAR_FLAG_DEPTH) != 0 && record.resource != nullptr && !record.resource->IsBuffer()) {
        if (Resource::DepthShadow* const shadow = record.resource->DepthShadowOf()) shadow->planeNewer.store(true, std::memory_order_relaxed);
    }
    if (TracingFrame()) {
        AbxLog("d3d12_x", "TRACE ClearDepthStencilView image %p flags 0x%x depth %g stencil %u (%u rects)",
               record.resource != nullptr && !record.resource->IsBuffer() ? static_cast<void*>(record.resource->Texture().image) : nullptr, static_cast<unsigned>(flags),
               static_cast<double>(depth), static_cast<unsigned>(stencil), count);
    }
}

void STDMETHODCALLTYPE CommandList::ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE view, const FLOAT color[4], UINT count, const D3D12_RECT* rects) {
    const TimedScope timed(TimedPart::ListCopies);
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
    if (record.resource != nullptr) record.resource->NoteListUse(true);
}
void CommandList::ClearUnorderedAccess(ID3D12Resource* resource, const VkClearColorValue& value, UINT count, const char* function) {
    EndPass();
    if (count != 0) AbxUnsupported(function, "an unordered access view clear with rectangles");
    Resource* texture = Ours(resource, function);
    if (texture == nullptr) AbxUnsupported(function, "an unordered access view clear of a resource AnyBox did not create");
    texture->NoteListUse(true);
    if (texture->IsBuffer()) {
        if (value.uint32[1] != value.uint32[0] || value.uint32[2] != value.uint32[0] || value.uint32[3] != value.uint32[0]) {
            AbxUnsupported(function, "a buffer clear whose four values differ: the view's format, which decides the pattern, is not read yet");
        }
        const UINT64 bytes = texture->Width() & ~UINT64{3};
        const TitleAddressSpace::Naming naming(function);
        const std::uint64_t destination = texture->GetGPUVirtualAddress();
        const bool throughTable = bytes != 0 && FillThroughTable(destination, bytes, value.uint32[0], function);
        if (TracingFrame()) {
            AbxLog("d3d12_x", "TRACE %s: %llu bytes at title 0x%llx filled with 0x%08x, %s | list %04x", function, static_cast<unsigned long long>(bytes),
                   static_cast<unsigned long long>(destination), value.uint32[0], throughTable ? "through the page table" : "naming the memory as a buffer",
                   static_cast<unsigned>((reinterpret_cast<std::uintptr_t>(this) >> 4) & 0xFFFFu));
        }
        if (throughTable) return;
        const std::optional<BufferRef> range = texture->BufferSpan(0, bytes);
        if (!range || range->size < bytes || (range->offset & 3) != 0) AbxUnsupported(function, "a buffer clear of a range that is not one GPU-visible, 4-byte aligned range");
        if (bytes != 0) context_.device.Vk().cmdFillBuffer(commands_, range->buffer, range->offset, bytes, value.uint32[0]);
        static std::atomic<bool> buffers{false};
        if (!buffers.exchange(true)) {
            AbxLog("d3d12_x", "unordered access view clears of buffers fill the whole buffer with the 32-bit value when all four values are equal (first: %llu bytes, value 0x%08x "
                              "by %s); the view's own range and format are not read",
                   static_cast<unsigned long long>(bytes), value.uint32[0], function);
        }
        return;
    }
    const D3D12_RESOURCE_DESC& desc = texture->Texture().desc;
    if (desc.MipLevels != 1 || desc.DepthOrArraySize != 1 || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        AbxUnsupported(function, "an unordered access view clear of a texture with several mips, layers or slices: which of them the view names is not read yet");
    }
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    context_.device.Vk().cmdClearColorImage(commands_, texture->Texture().image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "unordered access view clears fill the whole texture when it has one mip and one layer (first: %llux%u format %d by %s); other textures stop",
               static_cast<unsigned long long>(desc.Width), desc.Height, static_cast<int>(desc.Format), function);
    }
}

void STDMETHODCALLTYPE CommandList::ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource* resource, const UINT values[4], UINT count,
                                                                const D3D12_RECT*) {
    const char* function = "ID3D12GraphicsCommandList::ClearUnorderedAccessViewUint";
    if (values == nullptr) AbxUnsupported(function, "a clear without values");
    VkClearColorValue value{};
    for (int index = 0; index < 4; ++index) value.uint32[index] = values[index];
    ClearUnorderedAccess(resource, value, count, function);
}
void STDMETHODCALLTYPE CommandList::ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource* resource, const float values[4], UINT count,
                                                                 const D3D12_RECT*) {
    const char* function = "ID3D12GraphicsCommandList::ClearUnorderedAccessViewFloat";
    if (values == nullptr) AbxUnsupported(function, "a clear without values");
    VkClearColorValue value{};
    for (int index = 0; index < 4; ++index) value.float32[index] = values[index];
    ClearUnorderedAccess(resource, value, count, function);
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
    const TimedScope timed(TimedPart::ListWork);
    auto* ours = dynamic_cast<CommandSignature*>(signature);
    if (ours == nullptr) AbxUnsupported(function, "a command signature that AnyBox did not create");
    if (!ours->Signature()) AbxUnsupported(function, ("a command signature AnyBox cannot execute: " + ours->Unsupported()).c_str());
    const IndirectSignature& indirect = *ours->Signature();
    if (maxCount == 0) return;
    Resource* argumentBuffer = Ours(arguments, function);
    if (argumentBuffer == nullptr || !argumentBuffer->IsBuffer()) AbxUnsupported(function, "indirect arguments that are not in a buffer");
    const UINT64 span = static_cast<UINT64>(indirect.stride) * (maxCount - 1) + IndirectArgumentBytes(indirect.kind);
    const TitleAddressSpace::Naming naming(function);
    TableArguments tableArguments;
    bool argumentsThroughTable = false;
    if ((indirect.kind == IndirectKind::Dispatch || indirect.kind == IndirectKind::DispatchMesh) && context_.addresses != nullptr && context_.addresses->TableCommands() &&
        argumentOffset <= argumentBuffer->Width() && span <= argumentBuffer->Width() - argumentOffset && IndirectClamp::For(context_.device) != nullptr &&
        TableCopy::For(context_.device) != nullptr) {
        tableArguments.root = context_.addresses->RootTable();
        tableArguments.title = argumentBuffer->GetGPUVirtualAddress() + argumentOffset;
        const TitleAccess access{tableArguments.title, span, false};
        argumentsThroughTable = context_.addresses->TableCommand(&access, 1);
    }
    const TableArguments* const throughTable = argumentsThroughTable ? &tableArguments : nullptr;
    std::optional<BufferRef> argumentRef;
    if (!argumentsThroughTable) {
        argumentRef = argumentBuffer->BufferSpan(argumentOffset, span);
        if (!argumentRef || argumentRef->size < span) AbxUnsupported(function, "indirect arguments that are not one GPU-visible range");
    }
    const VkBuffer argumentHandle = argumentRef ? argumentRef->buffer : VK_NULL_HANDLE;
    const VkDeviceSize argumentAt = argumentRef ? argumentRef->offset : 0;
    std::optional<BufferRef> countRef;
    std::uint64_t countTitle = 0;
    if (count != nullptr) {
        Resource* countBuffer = Ours(count, function);
        if (countBuffer == nullptr || !countBuffer->IsBuffer()) AbxUnsupported(function, "an indirect count that is not in a buffer");
        countRef = countBuffer->BufferSpan(countOffset, 4);
        if (!countRef || countRef->size < 4) AbxUnsupported(function, "an indirect count that is not GPU-visible");
        countTitle = countBuffer->TitleAddress() + countOffset;
    }
    std::optional<BufferRef> checkedArguments;
    const auto traceArguments = [&](IndirectClamp& clamp, const ClampedArguments& checked, bool mesh) {
        if (!TracingFrame()) return;
        std::string label;
        for (const std::string& shader : pipeline_->Shaders()) label += " " + shader.substr(0, 19);
        clamp.Trace(commands_, checked, maxCount, indirect.stride, mesh, countRef ? countRef->buffer : VK_NULL_HANDLE, countRef ? countRef->offset : 0,
                    argumentBuffer->TitleAddress() + argumentOffset, std::move(label));
    };
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
            const std::optional<ClampedArguments> checked = clamp->RecordDispatches(commands_, argumentHandle, argumentAt, maxCount, indirect.stride, false, clampError, throughTable);
            Check(checked.has_value(), function, clampError);
            checkedArguments = BufferRef{checked->buffer, checked->offset, checked->size};
            traceArguments(*clamp, *checked, false);
            Recorder(function).BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, workPipeline_);
            pipelineBound_ = true;
            computeSetBound_ = false;
        }
        PushUserData(compute_, kComputeFirstRootSgpr, function);
        BindTitleSet(VK_PIPELINE_BIND_POINT_COMPUTE);
    } else if (indirect.kind == IndirectKind::DispatchMesh) {
        if (pipeline_ == nullptr || pipeline_->BindPoint() != VK_PIPELINE_BIND_POINT_GRAPHICS) AbxUnsupported(function, "an indirect mesh dispatch without a graphics pipeline state set");
        if (!pipeline_->Shaders().empty() && !pipeline_->HasStage("ms")) AbxUnsupported(function, "an indirect mesh dispatch with a pipeline that has no mesh shader");
        if (IndirectClamp* clamp = IndirectClamp::For(context_.device)) {
            std::string clampError;
            std::optional<ClampedArguments> checked;
            const bool ahead = ChecksAheadOfPass() && type_ == D3D12_COMMAND_LIST_TYPE_DIRECT && !TracingFrame();
            const bool grouped = rendering_ && checks_ != VK_NULL_HANDLE;
            if (ahead && (!grouped || checksWritten_)) {
                if (grouped) NoteChecksRestarted(*pipeline_);
                EndPass();
                OpenChecksAhead(function);
            }
            if (ahead && checks_ != VK_NULL_HANDLE) {
                checked = clamp->RecordBatched(checkCopies_, checks_, argumentHandle, argumentAt, maxCount, indirect.stride, true, clampError, throughTable);
            } else {
                EndPass();
                checked = clamp->RecordDispatches(commands_, argumentHandle, argumentAt, maxCount, indirect.stride, true, clampError, throughTable);
                computeSetBound_ = false;
            }
            Check(checked.has_value(), function, clampError);
            checkedArguments = BufferRef{checked->buffer, checked->offset, checked->size};
            traceArguments(*clamp, *checked, true);
        }
        if (!PrepareDraw(function)) return;
    } else {
        if (pipeline_ != nullptr && pipeline_->HasStage("ms")) AbxUnsupported(function, "an indirect draw with a mesh pipeline (its signature is not a mesh dispatch)");
        if (!PrepareDraw(function)) return;
        if (TracingFrame()) {
            const std::uint64_t argumentTitle = argumentBuffer->TitleAddress() + argumentOffset;
            const std::string countText = count != nullptr ? ", count at title memory" + TitleWordsText(context_.addresses, countTitle, 4) : std::string();
            AbxLog("d3d12_x", "TRACE     indirect draw: up to %u record(s) of %u bytes at title 0x%llx%s%s (title memory as it is while the list is recorded: what the GPU writes there "
                              "shows an earlier frame's values) | list %04x",
                   maxCount, indirect.stride, static_cast<unsigned long long>(argumentTitle),
                   TitleWordsText(context_.addresses, argumentTitle, IndirectArgumentBytes(indirect.kind)).c_str(), countText.c_str(),
                   static_cast<unsigned>((reinterpret_cast<std::uintptr_t>(this) >> 4) & 0xFFFFu));
            std::string everyShader;
            for (const std::string& shader : pipeline_->Shaders()) everyShader += shader + " ";
            const GraphicsDescription* description = pipeline_->Description();
            if (description != nullptr && WatchedShader(everyShader)) {
                const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d = description->desc;
                const D3D12_DEPTH_STENCIL_DESC& z = d.DepthStencilState;
                const D3D12_RENDER_TARGET_BLEND_DESC& b = d.BlendState.RenderTarget[0];
                AbxLog("d3d12_x",
                       "TRACE     state of the watched draw: depth %d write %d func %d | stencil %d read 0x%x write 0x%x front fail %d depthfail %d pass %d func %d back fail %d "
                       "depthfail %d pass %d func %d | blend %d src %d dst %d op %d mask 0x%x | cull %d fill %d front counter-clockwise %d | rt f%d dsv f%d samples %u | list %04x",
                       z.DepthEnable, z.DepthWriteMask, z.DepthFunc, z.StencilEnable, z.StencilReadMask, z.StencilWriteMask, z.FrontFace.StencilFailOp, z.FrontFace.StencilDepthFailOp,
                       z.FrontFace.StencilPassOp, z.FrontFace.StencilFunc, z.BackFace.StencilFailOp, z.BackFace.StencilDepthFailOp, z.BackFace.StencilPassOp, z.BackFace.StencilFunc,
                       b.BlendEnable, b.SrcBlend, b.DestBlend, b.BlendOp, b.RenderTargetWriteMask, d.RasterizerState.CullMode, d.RasterizerState.FillMode,
                       d.RasterizerState.FrontCounterClockwise, d.RTVFormats[0], d.DSVFormat, d.SampleDesc.Count,
                       static_cast<unsigned>((reinterpret_cast<std::uintptr_t>(this) >> 4) & 0xFFFFu));
            }
        }
    }
    std::string error;
    if (!checkedArguments && !argumentRef) AbxUnsupported(function, "indirect arguments read through the page table without the argument check");
    Check(Recorder(function).ExecuteIndirect(indirect, maxCount, checkedArguments ? *checkedArguments : *argumentRef, 0, countRef ? &*countRef : nullptr, 0, error), function, error);
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("d3d12_x", "ExecuteIndirect runs as Vulkan indirect %s (first: up to %u commands, stride %u%s)",
               indirect.kind == IndirectKind::Dispatch ? "dispatches" : indirect.kind == IndirectKind::DispatchMesh ? "mesh draws" : "draws", maxCount,
               indirect.stride, count != nullptr ? ", count from a buffer" : "");
    }
}

}
