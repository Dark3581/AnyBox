#include "PipelineObject.hpp"

#include "AbxApi.hpp"

#include "FrameTiming.hpp"
#include "LiveStats.hpp"
#include "PipelineCompiler.hpp"
#include "PipelineState.hpp"
#include "ThreadLimit.hpp"

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
#include <fstream>
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

std::wstring UsedPipelineFile() {
    const std::wstring bad = BadPipelineFile();
    return bad.substr(0, bad.find_last_of(L'\\')) + L"\\anybox_used_pipelines.txt";
}

class UsedPipelines {
public:
    static UsedPipelines& Instance() {
        static UsedPipelines* used = new UsedPipelines();
        return *used;
    }

    bool UsedBefore(const std::string& key) const { return enabled_ && before_.count(key) != 0; }

    void Note(const std::string& key) {
        if (!enabled_ || key.empty() || before_.count(key) != 0) return;
        const std::lock_guard guard(lock_);
        if (noted_.insert(key).second) fresh_.push_back(key);
    }

    void Save() {
        std::vector<std::string> fresh;
        {
            const std::lock_guard guard(lock_);
            fresh.swap(fresh_);
        }
        if (fresh.empty()) return;
        std::string text;
        for (const std::string& key : fresh) text += key + "\r\n";
        const HANDLE out = CreateFileW(UsedPipelineFile().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        CloseHandle(out);
    }

private:
    UsedPipelines() {
        const char* value = AbxSetting("d3d12_x.warm_pipelines");
        enabled_ = value == nullptr || std::strtoul(value, nullptr, 10) != 0;
        if (!enabled_) {
            AbxLog("d3d12_x", "pipelines bound in earlier runs are not built ahead (d3d12_x.warm_pipelines = 0)");
            return;
        }
        const HANDLE in = CreateFileW(UsedPipelineFile().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (in != INVALID_HANDLE_VALUE) {
            std::string text;
            char buffer[65536];
            DWORD read = 0;
            while (ReadFile(in, buffer, sizeof(buffer), &read, nullptr) && read != 0) text.append(buffer, read);
            CloseHandle(in);
            std::size_t start = 0;
            while (start < text.size()) {
                std::size_t end = text.find_first_of("\r\n", start);
                if (end == std::string::npos) end = text.size();
                if (end > start) before_.insert(text.substr(start, end - start));
                start = end + 1;
            }
        }
        AbxLog("d3d12_x",
               "%zu pipelines the title bound in earlier runs (anybox_used_pipelines.txt, written as pipelines are first bound) are built by the background threads as soon as "
               "the title creates them, so their first draw or dispatch does not wait for the build (d3d12_x.warm_pipelines = 0 turns this off)",
               before_.size());
        std::thread([this] {
            for (;;) {
                Sleep(30000);
                Save();
            }
        }).detach();
    }

    bool enabled_ = false;
    std::set<std::string> before_;
    std::mutex lock_;
    std::set<std::string> noted_;
    std::vector<std::string> fresh_;
};

int PipelineWaitMs() {
    static const int limit = [] {
        const char* value = AbxSetting("d3d12_x.pipeline_wait_ms");
        const unsigned long asked = value != nullptr ? std::strtoul(value, nullptr, 10) : 0;
        if (asked < 1 || asked > 600000) return 30000;
        AbxLog("d3d12_x", "a draw or dispatch waits at most %lu ms for a pipeline that is still being built and is skipped after that (d3d12_x.pipeline_wait_ms; 30000 when unset)", asked);
        return static_cast<int>(asked);
    }();
    return limit;
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
               "starting only while %llu GiB of commit is free; a draw or dispatch with a pipeline still being built waits for it up to 30 s (one over 2 MiB of SPIR-V is skipped "
               "and moved to the front instead)",
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
                    ready_.wait(guard, [this] { return !urgent_.empty() || !tasks_.empty() || !heavy_.empty(); });
                }
                std::deque<std::function<void()>>& lane = heavy ? heavy_ : !urgent_.empty() ? urgent_ : !tasks_.empty() ? tasks_ : heavy_;
                if (!heavy && &lane == &heavy_) {
                    static std::atomic<bool> reported{false};
                    if (!reported.exchange(true)) {
                        AbxLog("d3d12_x", "a pipeline over 2 MiB of SPIR-V is built by an idle worker: workers take those whenever their own queues are empty, so large graphics "
                                          "pipelines do not wait behind the build thread for large ones");
                    }
                }
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

bool ComputeOnFirstUse() {
    static const bool enabled = [] {
        const char* value = AbxSetting("d3d12_x.compute_pipelines_on_first_use");
        return value != nullptr && value[0] == '1' && value[1] == 0;
    }();
    return enabled;
}
constexpr std::uint64_t kRefusedBytes = 16ull << 20;
constexpr std::size_t kFormatVariants = 4;
constexpr std::size_t kModuleHeadBytes = 256u << 10;

bool SpecializeBufferFormats() {
    static const bool enabled = [] {
        const char* value = AbxSetting("d3d12_x.specialize_buffer_formats");
        const bool on = value != nullptr && value[0] == '1' && value[1] == 0;
        if (on) {
            AbxLog("d3d12_x", "compute modules over %llu MiB of SPIR-V that name buffer format sources are built for the word 3 their dispatch reads from title memory "
                              "(d3d12_x.specialize_buffer_formats = 1; off unless set): up to %zu builds a pipeline, one for each set of words, in the background; a dispatch "
                              "waits for none of them and is skipped until its build is done; more sets, or a word that cannot be read, use the module as stored",
                   static_cast<unsigned long long>(kHeavyBytes >> 20), kFormatVariants);
        }
        return on;
    }();
    return enabled;
}

std::vector<BufferFormatSource> ModuleFormatSources(const std::filesystem::path& module, std::string& why) {
    std::ifstream in(module, std::ios::binary);
    std::vector<std::uint32_t> words(kModuleHeadBytes / sizeof(std::uint32_t));
    in.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(kModuleHeadBytes));
    const std::size_t count = static_cast<std::size_t>(in.gcount()) / sizeof(std::uint32_t);
    return ReadBufferFormatSources(words.data(), count, why);
}

std::string FormatWordsText(const std::vector<std::uint32_t>& words) {
    std::string text;
    for (const std::uint32_t word : words) {
        char piece[16];
        std::snprintf(piece, sizeof(piece), "%s%08x", text.empty() ? "" : " ", word);
        text += piece;
    }
    return text;
}

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
    if (UsedPipelines::Instance().UsedBefore(PipelineKey(shaders_))) {
        static std::atomic<std::uint64_t> warmed{0};
        const std::uint64_t count = ++warmed;
        if (count == 1 || (count & (count - 1)) == 0) {
            AbxLog("d3d12_x", "a pipeline the title bound in an earlier run is built in the background from its creation (%llu so far)", static_cast<unsigned long long>(count));
        }
        AddRef();
        BuildQueue::Instance().Push(
            [this] {
                RunBuild();
                Release();
            },
            false, heavy_);
        return;
    }
    if (point_ == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            AbxLog("d3d12_x", "graphics pipelines are built when a draw first uses them, from the pipeline cache when it holds them, not in the background: a background build reads "
                              "and translates every shader module the title names, which competes with the title's own reads while it loads");
        }
        return;
    }
    if (ComputeOnFirstUse()) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            AbxLog("d3d12_x", "compute pipelines are built when a dispatch first uses them, not in the background (d3d12_x.compute_pipelines_on_first_use = 1): a pipeline the "
                              "title creates and never uses takes no build time and no driver memory; the first dispatch waits for its pipeline up to 30 s, and one over "
                              "2 MiB of SPIR-V is skipped until the build thread for those has it");
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

void PipelineStateObject::StartGenericBuild() {
    if (genericStarted_.exchange(true) || !genericBuild_) return;
    unbuilt_.clear();
    BuildInBackground(std::move(genericBuild_), genericBytes_);
}

VkPipeline PipelineStateObject::FormatVariant(const std::vector<std::uint32_t>& words, bool& pending, bool& generic, std::string& why) {
    pending = false;
    generic = false;
    why.clear();
    FormatVariantBuild* build = nullptr;
    {
        const std::lock_guard guard(variantLock_);
        const auto found = variants_.find(words);
        if (found != variants_.end()) {
            build = found->second.get();
        } else {
            if (variants_.size() >= kFormatVariants) {
                generic = true;
                why = "its dispatches read more than " + std::to_string(kFormatVariants) + " sets of buffer format words";
                return VK_NULL_HANDLE;
            }
            if (moduleBytes_ > kRefusedBytes) {
                why = "its shaders hold " + std::to_string(moduleBytes_ >> 20) + " MiB of SPIR-V, more than the " + std::to_string(kRefusedBytes >> 20) + " MiB AnyBox hands the driver";
                return VK_NULL_HANDLE;
            }
            const std::string key = PipelineKey(shaders_) + "|formats " + FormatWordsText(words);
            if (BadPipelines().count(key) != 0) {
                why = "this pipeline crashed the driver's shader compiler in an earlier run";
                return VK_NULL_HANDLE;
            }
            if (!computeSource_ || library_ == nullptr) {
                why = "the pipeline has no compute module";
                return VK_NULL_HANDLE;
            }
            build = variants_.emplace(words, std::make_unique<FormatVariantBuild>()).first->second.get();
            PipelineSource source = *computeSource_;
            source.constants = BufferFormatConstants(formatSources_, words);
            const std::size_t number = variants_.size();
            AbxLog("d3d12_x", "compute pipeline %s is built for buffer format words %s (build %zu of at most %zu, %llu KiB of SPIR-V, in the background)",
                   shaders_.empty() ? "(no shader)" : shaders_.front().c_str(), FormatWordsText(words).c_str(), number, kFormatVariants,
                   static_cast<unsigned long long>(moduleBytes_ >> 10));
            AddRef();
            BuildQueue::Instance().Push(
                [this, build, source, key] {
                    const std::size_t length = (std::min)(key.size(), sizeof(t_building) - 1);
                    std::memcpy(t_building, key.data(), length);
                    t_building[length] = 0;
                    const PipelineEntry entry = library_->Compute(source);
                    t_building[0] = 0;
                    if (entry.pipeline == VK_NULL_HANDLE) {
                        ++Stats().pipelinesFailed;
                        build->error = std::string("the pipeline for its buffer formats was ") + PipelineOriginName(entry.origin) + (entry.error.empty() ? "" : ": " + entry.error);
                        AbxLog("d3d12_x", "compute pipeline %s for buffer format words was not built: %s", shaders_.empty() ? "(no shader)" : shaders_.front().c_str(),
                               build->error.c_str());
                    } else {
                        ++Stats().pipelinesBuilt;
                        if (entry.origin == PipelineOrigin::Ready) ++Stats().pipelinesFromCache;
                        else Stats().pipelineCompileMs += static_cast<std::uint64_t>(entry.seconds * 1000.0);
                        {
                            const std::lock_guard guard(variantLock_);
                            if (!codeBaseNoted_.exchange(true)) {
                                usesCodeBase_ = entry.usesCodeBase;
                                codeBaseModules_ = entry.codeBaseModules;
                            }
                        }
                        AbxLog("d3d12_x", "compute pipeline %s for buffer format words is %s in %.1f s", shaders_.empty() ? "(no shader)" : shaders_.front().c_str(),
                               PipelineOriginName(entry.origin), entry.seconds);
                        build->pipeline.store(entry.pipeline, std::memory_order_release);
                    }
                    build->pending.store(false, std::memory_order_release);
                    Release();
                },
                true, heavy_);
        }
    }
    pending = build->pending.load(std::memory_order_acquire);
    if (pending) return VK_NULL_HANDLE;
    const VkPipeline pipeline = build->pipeline.load(std::memory_order_acquire);
    if (pipeline == VK_NULL_HANDLE) why = build->error;
    return pipeline;
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
    const TimedScope timed(TimedPart::PipelineWait);
    if (!started_.load(std::memory_order_acquire) && moduleBytes_ <= kQuickBytes) {
        const auto begin = std::chrono::steady_clock::now();
        RunBuild();
        static std::atomic<int> logged[2]{};
        if (logged[point_ == VK_PIPELINE_BIND_POINT_COMPUTE ? 0 : 1].fetch_add(1) < 12) {
            const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
            AbxLog("d3d12_x", "a %s pipeline was built on the recording thread (%llu KiB of SPIR-V) in %lld ms", point_ == VK_PIPELINE_BIND_POINT_COMPUTE ? "compute" : "graphics",
                   static_cast<unsigned long long>(moduleBytes_ >> 10), static_cast<long long>(took));
        }
    }
    Prioritize();
    const auto limit = std::chrono::milliseconds(ComputeOnFirstUse() || point_ != VK_PIPELINE_BIND_POINT_COMPUTE ? PipelineWaitMs() : 50);
    const auto begin = std::chrono::steady_clock::now();
    while (Pending() && std::chrono::steady_clock::now() - begin < limit) Sleep(1);
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
            if (entry.origin == PipelineOrigin::Ready) ++Stats().pipelinesFromCache;
            else Stats().pipelineCompileMs += static_cast<std::uint64_t>(entry.seconds * 1000.0);
            usesCodeBase_ = entry.usesCodeBase;
            writesMemory_.store(entry.writesMemory, std::memory_order_relaxed);
            codeBaseModules_ = entry.codeBaseModules;
            origin_ = entry.origin;
            if (entry.laneTurns != 0 || !entry.laneNote.empty()) {
                static std::atomic<std::uint64_t> split{0};
                static std::atomic<std::uint64_t> compiled{0};
                static std::atomic<std::uint64_t> left{0};
                if (!entry.laneNote.empty()) {
                    const std::uint64_t count = ++left;
                    if (count <= 8 || (count & (count - 1)) == 0) {
                        AbxLog("d3d12_x", "mesh shader %s runs its group as the store has it (d3d12_x.mesh_registers_in_memory could not split it into turns: %s; %llu so far)",
                               shaders_.empty() ? "(none)" : shaders_.front().substr(0, 16).c_str(), entry.laneNote.c_str(), static_cast<unsigned long long>(count));
                    }
                } else {
                    const std::uint64_t count = ++split;
                    const std::uint64_t built = entry.origin == PipelineOrigin::Ready ? compiled.load() : ++compiled;
                    if (count == 1 || (count & (count - 1)) == 0) {
                        AbxLog("d3d12_x", "mesh pipelines whose groups run in turns (d3d12_x.mesh_registers_in_memory): %llu so far, %llu of them compiled in this run and the rest from "
                                          "the pipeline cache (latest %s: %u turns, %zu regions between barriers, %s in %.1f s)",
                               static_cast<unsigned long long>(count), static_cast<unsigned long long>(built), shaders_.empty() ? "(none)" : shaders_.front().substr(0, 16).c_str(),
                               entry.laneTurns, entry.laneRegions, PipelineOriginName(entry.origin), entry.seconds);
                    }
                }
            }
            pipeline_.store(entry.pipeline, std::memory_order_release);
        }
        build_ = nullptr;
        pending_.store(false, std::memory_order_release);
    }
}

void PipelineStateObject::NoteBound() {
    const std::uint32_t minute = Stats().pipelineMinute.load(std::memory_order_relaxed);
    const std::uint32_t before = boundMinute_.exchange(minute, std::memory_order_relaxed);
    if (before == minute) return;
    if (before == 0) UsedPipelines::Instance().Note(PipelineKey(shaders_));
    ++Stats().pipelinesBoundThisMinute;
    if (before != 0 && minute - before > 2) ++Stats().pipelinesBackThisMinute;
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

bool PipelineStateObject::GroupSize(std::array<std::uint32_t, 3>& size, std::string& error) {
    const std::lock_guard guard(threadLock_);
    if (!groupSize_ && groupSizeError_.empty()) {
        if (!computeSource_) {
            groupSizeError_ = "the pipeline has no compute module";
        } else {
            bool usesCodeBase = false;
            const std::optional<std::vector<std::uint32_t>> words = ReadTitleModule(computeSource_->module, usesCodeBase, groupSizeError_);
            if (words) groupSize_ = ComputeGroupSize(*words, groupSizeError_);
            if (!groupSize_ && groupSizeError_.empty()) groupSizeError_ = "the module could not be read";
        }
    }
    if (!groupSize_) {
        error = groupSizeError_;
        return false;
    }
    size = *groupSize_;
    return true;
}

VkPipeline PipelineStateObject::ThreadLimited(std::uint32_t limitRegister, std::string& error) {
    const std::lock_guard guard(threadLock_);
    if (limitRegister_ && *limitRegister_ != limitRegister) {
        error = "a thread-limited pipeline asked for with two different limit registers";
        return VK_NULL_HANDLE;
    }
    if (!limitRegister_) {
        limitRegister_ = limitRegister;
        if (!computeSource_ || library_ == nullptr) {
            threadLimitedError_ = "the pipeline has no compute module";
        } else {
            PipelineSource source = *computeSource_;
            source.threadLimited = true;
            source.limitRegister = limitRegister;
            const PipelineEntry entry = library_->Compute(source);
            threadLimited_ = entry.pipeline;
            if (threadLimited_ == VK_NULL_HANDLE) threadLimitedError_ = entry.error.empty() ? std::string("the thread-limited pipeline was not built") : entry.error;
            AbxLog("d3d12_x", "compute pipeline %s got a second pipeline whose threads past a thread count return at once, for dispatches that count threads and end in a partial group: %s",
                   shaders_.empty() ? "(no shader)" : shaders_.front().c_str(), threadLimited_ != VK_NULL_HANDLE ? PipelineOriginName(entry.origin) : threadLimitedError_.c_str());
        }
    }
    if (threadLimited_ == VK_NULL_HANDLE) error = threadLimitedError_;
    return threadLimited_;
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
    created->computeSource_ = *source;
    created->library_ = &context.library;
    std::function<PipelineEntry()> build = [&library = context.library, source = *source] { return library.Compute(source); };
    const std::uint64_t moduleBytes = ModuleBytes(source->module);
    if (SpecializeBufferFormats() && moduleBytes > kHeavyBytes) {
        std::string why;
        std::vector<BufferFormatSource> sources = ModuleFormatSources(source->module, why);
        if (!why.empty()) {
            static std::atomic<int> reported{0};
            if (reported++ < 10) AbxLog("d3d12_x", "compute module %s is built as stored: %s", source->key.c_str(), why.c_str());
        }
        if (!sources.empty()) {
            static std::atomic<int> reported{0};
            if (reported++ < 20) {
                AbxLog("d3d12_x", "compute pipeline cs %s (%llu KiB of SPIR-V) waits for its first dispatch, which reads word 3 of %zu buffer descriptors for it", source->key.c_str(),
                       static_cast<unsigned long long>(moduleBytes >> 10), sources.size());
            }
            created->formatSources_ = std::move(sources);
            created->genericBuild_ = std::move(build);
            created->genericBytes_ = moduleBytes;
            created->moduleBytes_ = moduleBytes;
            created->heavy_ = moduleBytes > kHeavyBytes;
            created->unbuilt_ = "its pipeline is built for the buffer formats its dispatch reads";
            return Finish(created, riid, state, error);
        }
    }
    created->BuildInBackground(std::move(build), moduleBytes);
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
