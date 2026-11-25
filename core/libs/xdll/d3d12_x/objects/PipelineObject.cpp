#include "PipelineObject.hpp"

#include "AbxApi.hpp"

#include "LiveStats.hpp"
#include "PipelineState.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <string>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

namespace abx::d3d12x {

namespace {

thread_local char t_building[1024] = {};

std::wstring BadPipelineFile() {
    wchar_t module[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    std::wstring path(module, length);
    const std::size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash)) + L"\\anybox_bad_pipelines.txt";
}

LONG CALLBACK RecordCompilerCrash(EXCEPTION_POINTERS* info) {
    if (info == nullptr || info->ExceptionRecord == nullptr || info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || t_building[0] == 0) return EXCEPTION_CONTINUE_SEARCH;
    static wchar_t path[MAX_PATH + 32] = {};
    if (path[0] == 0) {
        const std::wstring file = BadPipelineFile();
        if (file.size() >= MAX_PATH + 31) return EXCEPTION_CONTINUE_SEARCH;
        for (std::size_t index = 0; index < file.size(); ++index) path[index] = file[index];
    }
    const HANDLE out = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        DWORD length = 0;
        while (length < sizeof(t_building) && t_building[length] != 0) ++length;
        WriteFile(out, t_building, length, &written, nullptr);
        WriteFile(out, "\r\n", 2, &written, nullptr);
        CloseHandle(out);
    }
    t_building[0] = 0;
    return EXCEPTION_CONTINUE_SEARCH;
}

const std::set<std::string>& BadPipelines() {
    static const std::set<std::string> bad = [] {
        std::set<std::string> keys;
        const HANDLE in = CreateFileW(BadPipelineFile().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (in != INVALID_HANDLE_VALUE) {
            std::string text;
            char buffer[4096];
            DWORD read = 0;
            while (ReadFile(in, buffer, sizeof(buffer), &read, nullptr) && read != 0) text.append(buffer, read);
            CloseHandle(in);
            std::size_t start = 0;
            while (start < text.size()) {
                std::size_t end = text.find_first_of("\r\n", start);
                if (end == std::string::npos) end = text.size();
                if (end > start) keys.insert(text.substr(start, end - start));
                start = end + 1;
            }
        }
        AddVectoredExceptionHandler(1, &RecordCompilerCrash);
        if (!keys.empty()) AbxLog("d3d12_x", "%zu pipelines that crashed the driver's shader compiler in earlier runs are left unbuilt (anybox_bad_pipelines.txt)", keys.size());
        return keys;
    }();
    return bad;
}

std::string PipelineKey(const std::vector<std::string>& shaders) {
    std::string key;
    for (const std::string& shader : shaders) key += (key.empty() ? "" : "|") + shader;
    return key;
}

class BuildQueue {
public:
    static BuildQueue& Instance() {
        static BuildQueue* queue = new BuildQueue();
        return *queue;
    }

    void Push(std::function<void()> task, bool urgent, bool heavy) {
        {
            const std::lock_guard guard(lock_);
            if (heavy) {
                if (urgent) heavy_.push_front(std::move(task));
                else heavy_.push_back(std::move(task));
            } else {
                (urgent ? urgent_ : tasks_).push_back(std::move(task));
            }
            Stats().pipelinesQueued.store(tasks_.size() + urgent_.size() + heavy_.size());
        }
        ready_.notify_all();
    }

private:
    BuildQueue() {
        const unsigned concurrency = std::thread::hardware_concurrency();
        unsigned workers = (std::max)(concurrency / 2, 2u);
        if (const char* value = AbxSetting("d3d12_x.pipeline_workers")) {
            const unsigned long asked = std::strtoul(value, nullptr, 10);
            if (asked >= 1 && asked <= 64) workers = static_cast<unsigned>(asked);
        }
        for (unsigned index = 0; index < workers; ++index) std::thread([this] { Run(false); }).detach();
        std::thread([this] { Run(true); }).detach();
        AbxLog("d3d12_x",
               "pipelines are built in the background by %u threads at lowest priority plus one for pipelines over 2 MiB of SPIR-V (d3d12_x.pipeline_workers; %u hardware threads), each "
               "starting only while %llu GiB of commit is free; a draw or dispatch with a pipeline still being built is skipped and moves that pipeline to the front",
               workers, concurrency, static_cast<unsigned long long>(kFreeCommit >> 30));
    }

    void Run(bool heavy) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock guard(lock_);
                if (heavy) {
                    ready_.wait(guard, [this] { return !heavy_.empty(); });
                } else {
                    ready_.wait(guard, [this] { return !urgent_.empty() || !tasks_.empty(); });
                }
                std::deque<std::function<void()>>& lane = heavy ? heavy_ : urgent_.empty() ? tasks_ : urgent_;
                task = std::move(lane.front());
                lane.pop_front();
                Stats().pipelinesQueued.store(tasks_.size() + urgent_.size() + heavy_.size());
            }
            WaitForCommit();
            ++Stats().pipelinesBuilding;
            task();
            --Stats().pipelinesBuilding;
            std::size_t queued = 0;
            {
                const std::lock_guard guard(lock_);
                queued = tasks_.size() + urgent_.size() + heavy_.size();
            }
            const std::uint64_t done = ++done_;
            if (done % 500 == 0) AbxLog("d3d12_x", "background pipeline builds: %llu done, %zu queued", static_cast<unsigned long long>(done), queued);
        }
    }

    std::atomic<std::uint64_t> done_{0};

    static void WaitForCommit() {
        for (;;) {
            MEMORYSTATUSEX status{sizeof(status)};
            if (!GlobalMemoryStatusEx(&status) || status.ullAvailPageFile >= kFreeCommit) return;
            Sleep(50);
        }
    }

    static constexpr std::uint64_t kFreeCommit = 3ull << 30;

    std::mutex lock_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> tasks_;
    std::deque<std::function<void()>> urgent_;
    std::deque<std::function<void()>> heavy_;
};

constexpr std::uint64_t kHeavyBytes = 2ull << 20;
constexpr std::uint64_t kRefusedBytes = 16ull << 20;

std::uint64_t ModuleBytes(const std::filesystem::path& module) {
    std::error_code error;
    const std::uintmax_t bytes = std::filesystem::file_size(module, error);
    return error ? 0 : static_cast<std::uint64_t>(bytes);
}

HRESULT FromEntry(const PipelineEntry& entry, std::string& error) {
    if (entry.pipeline != VK_NULL_HANDLE) {
        return S_OK;
    }
    error = std::string("the pipeline was ") + PipelineOriginName(entry.origin) + (entry.error.empty() ? "" : ": " + entry.error);
    return entry.origin == PipelineOrigin::Refused ? E_OUTOFMEMORY : E_FAIL;
}

}

PipelineStateObject::PipelineStateObject(IUnknown* parent, VkPipelineBindPoint point, VkPipeline pipeline, PipelineOrigin origin, RootSignature* signature)
    : DeviceChildBase(parent), point_(point), pipeline_(pipeline), origin_(origin), signature_(signature) {
    signature_->AddRef();
}

void PipelineStateObject::BuildInBackground(std::function<PipelineEntry()> build, std::uint64_t moduleBytes) {
    if (moduleBytes > kRefusedBytes) {
        char text[200];
        std::snprintf(text, sizeof(text), "its shaders hold %llu MiB of SPIR-V, more than the %llu MiB AnyBox hands the driver (such modules took tens of GB to compile)",
                      static_cast<unsigned long long>(moduleBytes >> 20), static_cast<unsigned long long>(kRefusedBytes >> 20));
        unbuilt_ = text;
        ++Stats().pipelinesFailed;
        static std::atomic<int> reported{0};
        if (reported++ < 10) AbxLog("d3d12_x", "a pipeline is not built (%s): %s", shaders_.empty() ? "no shaders" : shaders_.front().c_str(), text);
        return;
    }
    if (BadPipelines().count(PipelineKey(shaders_)) != 0) {
        unbuilt_ = "this pipeline crashed the driver's shader compiler in an earlier run";
        ++Stats().pipelinesFailed;
        return;
    }
    heavy_ = moduleBytes > kHeavyBytes;
    moduleBytes_ = moduleBytes;
    build_ = std::move(build);
    pending_.store(true, std::memory_order_release);
    if (point_ == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            AbxLog("d3d12_x", "graphics pipelines are built when a draw first uses them, from the pipeline cache when it holds them, not in the background: a background build reads "
                              "and translates every shader module the title names, which competes with the title's own reads while it loads");
        }
        return;
    }
    AddRef();
    BuildQueue::Instance().Push(
        [this] {
            RunBuild();
            Release();
        },
        false, heavy_);
}

void PipelineStateObject::Prioritize() {
    if (!Alive()) AbxUnsupported("ID3D12PipelineState", "a pipeline state used after its last reference was released");
    if (!Pending() || started_.load(std::memory_order_acquire) || prioritized_.exchange(true)) return;
    AddRef();
    BuildQueue::Instance().Push(
        [this] {
            RunBuild();
            Release();
        },
        true, heavy_);
}

void PipelineStateObject::BuildNow() {
    constexpr std::uint64_t kQuickBytes = 512u << 10;
    if (!Pending() || heavy_) return;
    if (!started_.load(std::memory_order_acquire) && moduleBytes_ <= kQuickBytes) {
        const auto begin = std::chrono::steady_clock::now();
        RunBuild();
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 12) {
            const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
            AbxLog("d3d12_x", "a compute pipeline was built on the recording thread (%llu KiB of SPIR-V) in %lld ms", static_cast<unsigned long long>(moduleBytes_ >> 10),
                   static_cast<long long>(took));
        }
    }
    for (int waited = 0; Pending() && waited < 50; ++waited) Sleep(1);
}

void PipelineStateObject::RunBuild() {
    if (started_.exchange(true)) return;
    {
        const std::string key = PipelineKey(shaders_);
        const std::size_t length = (std::min)(key.size(), sizeof(t_building) - 1);
        std::memcpy(t_building, key.data(), length);
        t_building[length] = 0;
        const auto buildBegin = std::chrono::steady_clock::now();
        const PipelineEntry entry = build_();
        t_building[0] = 0;
        {
            const long long took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - buildBegin).count();
            static std::atomic<long long> totalMs{0};
            static std::atomic<std::uint64_t> slow{0};
            static std::atomic<std::uint64_t> all{0};
            totalMs += took;
            const std::uint64_t count = ++all;
            if (took >= 3000) {
                const std::uint64_t slowCount = ++slow;
                if (slowCount <= 40 || (slowCount & (slowCount - 1)) == 0) {
                    AbxLog("d3d12_x", "DIAG slow pipeline build %lld ms: %llu KiB of SPIR-V, %s, %zu stages, first %s (%llu slow of %llu, %lld s compiling in total)", took,
                           static_cast<unsigned long long>(moduleBytes_ >> 10), point_ == VK_PIPELINE_BIND_POINT_COMPUTE ? "compute" : "graphics", shaders_.size(),
                           shaders_.empty() ? "none" : shaders_.front().substr(0, 16).c_str(), static_cast<unsigned long long>(slowCount), static_cast<unsigned long long>(count),
                           totalMs.load() / 1000);
                }
            }
        }
        std::string error;
        if (FAILED(FromEntry(entry, error))) {
            ++Stats().pipelinesFailed;
            unbuilt_ = error;
            static std::atomic<int> reported{0};
            if (reported++ < 20) AbxLog("d3d12_x", "a pipeline built in the background failed (%s): %s", shaders_.empty() ? "no shaders" : shaders_.front().c_str(), error.c_str());
        } else {
            ++Stats().pipelinesBuilt;
            usesCodeBase_ = entry.usesCodeBase;
            origin_ = entry.origin;
            pipeline_.store(entry.pipeline, std::memory_order_release);
        }
        build_ = nullptr;
        pending_.store(false, std::memory_order_release);
    }
}

PipelineStateObject::~PipelineStateObject() {
    alive_ = 0;
    signature_->Release();
}

bool PipelineStateObject::Implements(REFIID riid) const {
    return DeviceChildBase::Implements(riid) || riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12PipelineState);
}

HRESULT STDMETHODCALLTYPE PipelineStateObject::GetCachedBlob(ID3DBlob** blob) {
    if (blob != nullptr) *blob = nullptr;
    return E_NOTIMPL;
}

RootSignature* PipelineStateObject::SignatureOf(ID3D12RootSignature* signature, std::string& error) {
    if (signature == nullptr) {
        error = "a pipeline state without a root signature: reading one embedded in the shader is not implemented";
        return nullptr;
    }
    RootSignature* ours = dynamic_cast<RootSignature*>(signature);
    if (ours == nullptr) error = "a root signature that AnyBox did not create";
    return ours;
}

namespace {

std::vector<std::uint8_t> CopyBytecode(const D3D12_SHADER_BYTECODE& code) {
    if (code.pShaderBytecode == nullptr || code.BytecodeLength == 0) return {};
    const auto* bytes = static_cast<const std::uint8_t*>(code.pShaderBytecode);
    return std::vector<std::uint8_t>(bytes, bytes + code.BytecodeLength);
}

D3D12_SHADER_BYTECODE View(const std::vector<std::uint8_t>& bytes) {
    return {bytes.empty() ? nullptr : bytes.data(), bytes.size()};
}

std::shared_ptr<const GraphicsDescription> Describe(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, const D3D12_SHADER_BYTECODE* mesh, const D3D12_SHADER_BYTECODE* amplification) {
    auto described = std::make_shared<GraphicsDescription>();
    described->desc = desc;
    described->vertex = CopyBytecode(desc.VS);
    described->pixel = CopyBytecode(desc.PS);
    described->geometry = CopyBytecode(desc.GS);
    if (mesh != nullptr) {
        described->mesh = CopyBytecode(*mesh);
        described->meshPipeline = true;
    }
    if (amplification != nullptr) described->amplification = CopyBytecode(*amplification);
    for (UINT index = 0; index < desc.InputLayout.NumElements && desc.InputLayout.pInputElementDescs != nullptr; ++index) {
        described->inputs.push_back(desc.InputLayout.pInputElementDescs[index]);
        described->semantics.emplace_back(desc.InputLayout.pInputElementDescs[index].SemanticName != nullptr ? desc.InputLayout.pInputElementDescs[index].SemanticName : "");
    }
    for (std::size_t index = 0; index < described->inputs.size(); ++index) described->inputs[index].SemanticName = described->semantics[index].c_str();
    described->desc.CachedPSO = {};
    described->desc.StreamOutput = {};
    return described;
}

const char* StageName(VkShaderStageFlagBits stage) {
    switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT: return "vs";
    case VK_SHADER_STAGE_FRAGMENT_BIT: return "ps";
    case VK_SHADER_STAGE_GEOMETRY_BIT: return "gs";
    case VK_SHADER_STAGE_MESH_BIT_EXT: return "ms";
    case VK_SHADER_STAGE_TASK_BIT_EXT: return "as";
    default: return "?";
    }
}

}

D3D12_GRAPHICS_PIPELINE_STATE_DESC GraphicsDescription::Rebuilt() const {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC rebuilt = desc;
    rebuilt.VS = View(vertex);
    rebuilt.PS = View(pixel);
    rebuilt.GS = View(geometry);
    rebuilt.HS = {};
    rebuilt.DS = {};
    rebuilt.InputLayout = {inputs.empty() ? nullptr : inputs.data(), static_cast<UINT>(inputs.size())};
    return rebuilt;
}

bool PipelineStateObject::HasStage(const char* stage) const {
    const std::string prefix = std::string(stage) + " ";
    for (const std::string& shader : shaders_) {
        if (shader.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

HRESULT PipelineStateObject::Finish(PipelineStateObject* created, REFIID riid, void** state, std::string& error) {
    std::string shaders;
    for (const std::string& shader : created->shaders_) shaders += " " + shader;
    AbxTrace("pipeline state %p created:%s%s", static_cast<void*>(created), shaders.empty() ? " (no shaders) " : shaders.c_str(),
             created->pipeline_ == VK_NULL_HANDLE ? (" unbuilt: " + created->unbuilt_).c_str() : "");
    const HRESULT result = created->QueryInterface(riid, state);
    created->Release();
    if (FAILED(result)) error = "the pipeline state does not implement the requested interface";
    return result;
}

HRESULT PipelineStateObject::CreateCompute(const PipelineContext& context, IUnknown* parent, const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, REFIID riid, void** state,
                                           std::string& error) {
    if (state == nullptr) {
        error = "no output for the pipeline state";
        return E_POINTER;
    }
    *state = nullptr;
    RootSignature* signature = SignatureOf(desc.pRootSignature, error);
    if (signature == nullptr) {
        return E_INVALIDARG;
    }
    const std::optional<PipelineSource> source = ComputePipelineSource(desc, context.store, error);
    if (!source) {
        return E_INVALIDARG;
    }
    PipelineStateObject* created = new PipelineStateObject(parent, VK_PIPELINE_BIND_POINT_COMPUTE, VK_NULL_HANDLE, PipelineOrigin{}, signature);
    created->shaders_.push_back("cs " + source->key);
    created->BuildInBackground([&library = context.library, source = *source] { return library.Compute(source); }, ModuleBytes(source->module));
    return Finish(created, riid, state, error);
}

HRESULT PipelineStateObject::CreateMesh(const PipelineContext& context, IUnknown* parent, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, const D3D12_SHADER_BYTECODE& mesh,
                                        const D3D12_SHADER_BYTECODE& amplification, REFIID riid, void** state, std::string& error) {
    if (state == nullptr) {
        error = "no output for the pipeline state";
        return E_POINTER;
    }
    *state = nullptr;
    RootSignature* signature = SignatureOf(desc.pRootSignature, error);
    if (signature == nullptr) {
        return E_INVALIDARG;
    }
    const std::optional<GraphicsRequest> request = MeshPipelineRequest(desc, mesh, amplification, context.store, error);
    if (!request) {
        return E_INVALIDARG;
    }
    PipelineStateObject* created = new PipelineStateObject(parent, VK_PIPELINE_BIND_POINT_GRAPHICS, VK_NULL_HANDLE, PipelineOrigin{}, signature);
    for (const GraphicsShaderSource& shader : request->shaders) created->shaders_.push_back(StageName(shader.stage) + std::string(" ") + shader.key);
    created->polygonMode_ = request->state.polygonMode;
    created->description_ = Describe(desc, &mesh, amplification.pShaderBytecode != nullptr ? &amplification : nullptr);
    std::uint64_t moduleBytes = 0;
    for (const GraphicsShaderSource& shader : request->shaders) moduleBytes += ModuleBytes(shader.module);
    created->BuildInBackground([&library = context.library, request = *request] { return library.Graphics(request); }, moduleBytes);
    return Finish(created, riid, state, error);
}

HRESULT PipelineStateObject::CreateUnbuilt(IUnknown* parent, VkPipelineBindPoint point, ID3D12RootSignature* signature, std::string reason, REFIID riid, void** state,
                                           std::string& error) {
    if (state == nullptr) {
        error = "no output for the pipeline state";
        return E_POINTER;
    }
    *state = nullptr;
    RootSignature* ours = SignatureOf(signature, error);
    if (ours == nullptr) {
        return E_INVALIDARG;
    }
    PipelineStateObject* created = new PipelineStateObject(parent, point, VK_NULL_HANDLE, PipelineOrigin{}, ours);
    created->unbuilt_ = std::move(reason);
    return Finish(created, riid, state, error);
}

HRESULT PipelineStateObject::CreateGraphics(const PipelineContext& context, IUnknown* parent, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, REFIID riid, void** state,
                                            std::string& error) {
    if (state == nullptr) {
        error = "no output for the pipeline state";
        return E_POINTER;
    }
    *state = nullptr;
    RootSignature* signature = SignatureOf(desc.pRootSignature, error);
    if (signature == nullptr) {
        return E_INVALIDARG;
    }
    const std::optional<GraphicsRequest> request = GraphicsPipelineRequest(desc, context.store, error);
    if (!request) {
        return E_INVALIDARG;
    }
    PipelineStateObject* created = new PipelineStateObject(parent, VK_PIPELINE_BIND_POINT_GRAPHICS, VK_NULL_HANDLE, PipelineOrigin{}, signature);
    for (const GraphicsShaderSource& shader : request->shaders) created->shaders_.push_back(StageName(shader.stage) + std::string(" ") + shader.key);
    created->polygonMode_ = request->state.polygonMode;
    created->description_ = Describe(desc, nullptr, nullptr);
    std::uint64_t moduleBytes = 0;
    for (const GraphicsShaderSource& shader : request->shaders) moduleBytes += ModuleBytes(shader.module);
    created->BuildInBackground([&library = context.library, request = *request] { return library.Graphics(request); }, moduleBytes);
    return Finish(created, riid, state, error);
}

}
