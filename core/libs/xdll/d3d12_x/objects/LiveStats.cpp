#include "LiveStats.hpp"

#include "AbxApi.hpp"
#include "DiscordPresence.hpp"
#include "FrameTiming.hpp"
#include "Presenter.hpp"
#include "Resource.hpp"
#include "VulkanDevice.hpp"

#include <windows.h>
#include <dxgi1_4.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace abx::d3d12x {

namespace {

bool Enabled() {
    const char* value = AbxSetting("d3d12_x.stats");
    return value == nullptr || std::string_view(value) != "0";
}

std::wstring Widen(const std::string& text);

std::wstring ConfiguredTitle() {
    const char* value = AbxSetting("d3d12_x.window_title");
    return value != nullptr ? Widen(value) : std::wstring();
}

IDXGIAdapter3* VideoAdapter() {
    using Create = HRESULT(WINAPI*)(REFIID, void**);
    const HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (dxgi == nullptr) return nullptr;
    const auto create = reinterpret_cast<Create>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
    IDXGIFactory1* factory = nullptr;
    if (create == nullptr || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return nullptr;
    IDXGIAdapter3* best = nullptr;
    SIZE_T bestMemory = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        IDXGIAdapter3* third = nullptr;
        if (desc.DedicatedVideoMemory > bestMemory && SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&third)))) {
            if (best != nullptr) best->Release();
            best = third;
            bestMemory = desc.DedicatedVideoMemory;
        }
        adapter->Release();
    }
    factory->Release();
    return best;
}

bool FitWindowEnabled() {
    const char* value = AbxSetting("d3d12_x.fit_window");
    return value == nullptr || std::string_view(value) != "0";
}

void FitToPrimaryMonitor(HWND window) {
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor)) return;
    const LONG style = GetWindowLongW(window, GWL_STYLE);
    const LONG extended = GetWindowLongW(window, GWL_EXSTYLE);
    RECT frame{0, 0, 1000, 1000};
    if (!AdjustWindowRectEx(&frame, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(extended))) return;
    const LONG frameWidth = (frame.right - frame.left) - 1000;
    const LONG frameHeight = (frame.bottom - frame.top) - 1000;
    const LONG workWidth = monitor.rcWork.right - monitor.rcWork.left;
    const LONG workHeight = monitor.rcWork.bottom - monitor.rcWork.top;
    const LONG availableWidth = workWidth - frameWidth;
    const LONG availableHeight = workHeight - frameHeight;
    if (availableWidth <= 0 || availableHeight <= 0) return;
    LONG clientWidth = availableWidth;
    LONG clientHeight = clientWidth * 9 / 16;
    if (clientHeight > availableHeight) {
        clientHeight = availableHeight;
        clientWidth = clientHeight * 16 / 9;
    }
    const LONG width = clientWidth + frameWidth;
    const LONG height = clientHeight + frameHeight;
    const LONG x = monitor.rcWork.left + (workWidth - width) / 2;
    const LONG y = monitor.rcWork.top + (workHeight - height) / 2;
    if (IsZoomed(window)) ShowWindowAsync(window, SW_RESTORE);
    SetWindowPos(window, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    AbxLog("d3d12_x", "the game window is fitted to the primary monitor's work area: %ldx%ld client area at (%ld, %ld) (d3d12_x.fit_window = 0 leaves it as the title made it)",
           clientWidth, clientHeight, x, y);
}

BOOL CALLBACK SecondMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM found) {
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(monitor, &info) && (info.dwFlags & MONITORINFOF_PRIMARY) == 0) {
        *reinterpret_cast<RECT*>(found) = info.rcWork;
        return FALSE;
    }
    return TRUE;
}

void KeepConsoleAside() {
    const HWND console = GetConsoleWindow();
    if (console == nullptr) return;
    RECT second{};
    EnumDisplayMonitors(nullptr, nullptr, &SecondMonitor, reinterpret_cast<LPARAM>(&second));
    if (second.right > second.left) {
        SetWindowPos(console, HWND_BOTTOM, second.left + 40, second.top + 40, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    } else {
        SetWindowPos(console, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }
}

HANDLE StatsConsole() {
    if (GetConsoleWindow() == nullptr && !AllocConsole()) return INVALID_HANDLE_VALUE;
    SetConsoleTitleW(L"AnyBox stats");
    KeepConsoleAside();
    return CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

std::wstring Widen(const std::string& text) {
    std::wstring wide(text.size(), L' ');
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), static_cast<int>(wide.size()));
    wide.resize(length > 0 ? static_cast<std::size_t>(length) : 0);
    return wide;
}

struct TimingSnapshot {
    std::uint64_t presents = 0;
    std::uint64_t nanoseconds[kTimedParts] = {};
    std::uint64_t calls[kTimedParts] = {};
    std::uint64_t renderPasses = 0;
    std::uint64_t barriers = 0;
    std::uint64_t checks = 0;
    std::uint64_t checksAhead = 0;
    std::uint64_t checkBatches = 0;
    std::uint64_t checkBatchesRestarted = 0;
    std::chrono::steady_clock::time_point at = std::chrono::steady_clock::now();
};

void ReportFrameTiming(TimingSnapshot& last, std::uint64_t presents) {
    TimingSnapshot now;
    now.presents = presents;
    for (std::size_t index = 0; index < kTimedParts; ++index) {
        now.nanoseconds[index] = g_frameTiming.nanoseconds[index].load(std::memory_order_relaxed);
        now.calls[index] = g_frameTiming.calls[index].load(std::memory_order_relaxed);
    }
    now.renderPasses = RecordedCommands().renderPasses.load(std::memory_order_relaxed);
    now.barriers = RecordedCommands().pipelineBarriers.load(std::memory_order_relaxed);
    now.checks = Stats().clampDispatches.load(std::memory_order_relaxed);
    now.checksAhead = Stats().clampsAhead.load(std::memory_order_relaxed);
    now.checkBatches = Stats().clampBatches.load(std::memory_order_relaxed);
    now.checkBatchesRestarted = Stats().clampBatchesRestarted.load(std::memory_order_relaxed);
    const std::uint64_t frames = now.presents - last.presents;
    if (frames != 0) {
        const double share = 1.0 / static_cast<double>(frames);
        const auto ms = [&](TimedPart part) {
            const std::size_t index = static_cast<std::size_t>(part);
            return static_cast<double>(now.nanoseconds[index] - last.nanoseconds[index]) / 1.0e6 * share;
        };
        const auto calls = [&](TimedPart part) {
            const std::size_t index = static_cast<std::size_t>(part);
            return static_cast<double>(now.calls[index] - last.calls[index]) * share;
        };
        const double frameMs = std::chrono::duration<double, std::milli>(now.at - last.at).count() * share;
        AbxLog("d3d12_x",
               "frame time 1/3: %llu frames of %.1f ms each; wall-clock ms a frame by part, summed over all threads (parts can overlap; brackets are inside the part before "
               "them): present %.1f [waiting for lists its queue still held %.1f, waiting for the window's next image %.1f, the frame's blit submitted, waited for and queued "
               "%.1f, resting for the desktop %.1f] | presents shown later in their queue's order %.1f (%.2f a frame) | ExecuteCommandLists %.1f in %.1f calls "
               "[vkQueueSubmit2 with its lock %.1f, of it waiting for the lock %.1f] | recording draws and dispatches %.1f [waiting for pipelines %.1f in %.2f "
               "waits], barriers %.1f, copies and clears %.1f, list close %.1f in %.1f lists",
               static_cast<unsigned long long>(frames), frameMs, ms(TimedPart::Present), ms(TimedPart::PresentHeld), ms(TimedPart::PresentAcquire), ms(TimedPart::PresentSync),
               ms(TimedPart::DesktopRest), ms(TimedPart::PresentLater), calls(TimedPart::PresentLater), ms(TimedPart::Execute), calls(TimedPart::Execute),
               ms(TimedPart::QueueSubmit), ms(TimedPart::QueueLockWait), ms(TimedPart::ListWork), ms(TimedPart::PipelineWait), calls(TimedPart::PipelineWait),
               ms(TimedPart::ListBarriers), ms(TimedPart::ListCopies), ms(TimedPart::ListClose), calls(TimedPart::ListClose));
        AbxLog("d3d12_x",
               "frame time 2/3: title textures made %.1f (%.2f a frame), refreshed %.1f [waiting for the texture lock %.1f, upload batches submitted and waited for %.1f, "
               "replaced textures released %.1f], flushed %.1f | readback %.1f in %.2f passes [writing title memory %.1f in %.2f writes] | GPU page mapping %.1f (%.2f calls a "
               "frame) | waits "
               "for the GPU to finish what it has: one-shot submits, the present's among them, %.1f (%.2f a frame), address space changes %.1f (%.2f a frame) | lists "
               "held back by memory waits %.1f (%.2f a frame)",
               ms(TimedPart::TextureMake), calls(TimedPart::TextureMake), ms(TimedPart::TextureRefresh), ms(TimedPart::RefreshLock), ms(TimedPart::RefreshSubmit),
               ms(TimedPart::RefreshRelease), ms(TimedPart::TextureFlush), ms(TimedPart::Readback), calls(TimedPart::Readback), ms(TimedPart::ReadbackWrite),
               calls(TimedPart::ReadbackWrite), ms(TimedPart::GpuPages), calls(TimedPart::GpuPages), ms(TimedPart::SyncSubmit), calls(TimedPart::SyncSubmit), ms(TimedPart::AddressIdleWait),
               calls(TimedPart::AddressIdleWait), ms(TimedPart::MemoryWaitHeld), calls(TimedPart::MemoryWaitHeld));
        const auto perFrame = [&](std::uint64_t TimingSnapshot::*field) { return static_cast<double>(now.*field - last.*field) * share; };
        AbxLog("d3d12_x",
               "frame time 3/3: commands recorded a frame: render passes begun %.1f, pipeline barriers %.1f (both counted in every command buffer AnyBox records) | indirect "
               "argument checks %.1f, %.1f of them ahead of their render pass in %.1f groups (d3d12_x.indirect_checks_ahead; %.1f of the groups begun inside a pass because a draw "
               "before them in it may write memory), the rest recorded in place, where a mesh draw's render pass is ended and begun again around it",
               perFrame(&TimingSnapshot::renderPasses), perFrame(&TimingSnapshot::barriers), perFrame(&TimingSnapshot::checks), perFrame(&TimingSnapshot::checksAhead),
               perFrame(&TimingSnapshot::checkBatches), perFrame(&TimingSnapshot::checkBatchesRestarted));
    }
    last = now;
}

void Run() {
    const bool showStats = Enabled();
    long long lastDecade = 0;
    long long lastMinute = 0;
    TimingSnapshot timing;
    const std::wstring configuredTitle = ConfiguredTitle();
    const HANDLE console = showStats ? StatsConsole() : INVALID_HANDLE_VALUE;
    LiveStats& stats = Stats();
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t lastPresents = 0;
    std::uint64_t lastDraws = 0;
    std::uint64_t lastDispatches = 0;
    std::uint64_t lastSkipped = 0;
    std::uint64_t lastBuilt = 0;
    std::wstring baseTitle;
    std::wstring shownTitle;
    HWND window = nullptr;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const std::uint64_t presents = stats.presents.load();
        const std::uint64_t draws = stats.draws.load();
        const std::uint64_t dispatches = stats.dispatches.load();
        const std::uint64_t skipped = stats.skippedBuilding.load() + stats.skippedOther.load();
        const std::uint64_t built = stats.pipelinesBuilt.load();
        MEMORYSTATUSEX memory{sizeof(memory)};
        GlobalMemoryStatusEx(&memory);
        static IDXGIAdapter3* const adapter = VideoAdapter();
        DXGI_QUERY_VIDEO_MEMORY_INFO video{};
        if (adapter != nullptr) adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &video);
        stats.videoUsage.store(video.CurrentUsage, std::memory_order_relaxed);
        stats.videoBudget.store(video.Budget, std::memory_order_relaxed);
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
        char line[512];
        std::snprintf(line, sizeof(line),
                      "%02lld:%02lld:%02lld | %llu fps | draws %llu/s, dispatches %llu/s, skipped %llu/s | pipelines %llu built (+%llu/s, %llu from cache, %llu s compiling), %llu building, %llu queued, "
                      "%llu failed | textures %llu, %.2f GB | VRAM %.2f of %.2f GB | free commit %.1f GB",
                      static_cast<long long>(seconds / 3600), static_cast<long long>(seconds / 60 % 60), static_cast<long long>(seconds % 60),
                      static_cast<unsigned long long>(presents - lastPresents), static_cast<unsigned long long>(draws - lastDraws),
                      static_cast<unsigned long long>(dispatches - lastDispatches), static_cast<unsigned long long>(skipped - lastSkipped),
                      static_cast<unsigned long long>(built), static_cast<unsigned long long>(built - lastBuilt),
                      static_cast<unsigned long long>(stats.pipelinesFromCache.load()), static_cast<unsigned long long>(stats.pipelineCompileMs.load() / 1000),
                      static_cast<unsigned long long>(stats.pipelinesBuilding.load()), static_cast<unsigned long long>(stats.pipelinesQueued.load()),
                      static_cast<unsigned long long>(stats.pipelinesFailed.load()), static_cast<unsigned long long>(stats.textureCount.load()),
                      static_cast<double>(stats.textureBytes.load()) / (1ull << 30), static_cast<double>(video.CurrentUsage) / (1ull << 30), static_cast<double>(video.Budget) / (1ull << 30),
                      static_cast<double>(memory.ullAvailPageFile) / (1ull << 30));
        const bool tenth = seconds / 10 != lastDecade;
        const bool minute = seconds / 60 != lastMinute;
        lastDecade = seconds / 10;
        lastMinute = seconds / 60;
        if (showStats && tenth) {
            AbxLog("d3d12_x", "stats %s", line);
            ReportFrameTiming(timing, presents);
        }
        if (showStats && minute) {
            AbxLog("d3d12_x", "written targets of 4 MiB and more still at the normal memory priority because no command list has named them (what the system moves out first when video "
                   "memory runs over): %llu MiB in %llu",
                   static_cast<unsigned long long>(stats.unusedTargetBytes.load() >> 20), static_cast<unsigned long long>(stats.unusedTargetCount.load()));
            const auto bufferKib = [&](const std::atomic<std::uint64_t>* values, int kind) { return static_cast<unsigned long long>(values[kind].load() >> 10); };
            AbxLog("d3d12_x",
                   "buffers at title addresses (system memory, which shaders reach over the bus): the GPU may write %llu KiB in %llu (%llu KiB in buffers of 64 KiB and more, "
                   "%llu KiB as whole 64 KiB pages) | read only %llu KiB in %llu (%llu KiB in buffers of 64 KiB and more, %llu KiB as whole 64 KiB pages)",
                   bufferKib(stats.titleBufferBytes, 1), static_cast<unsigned long long>(stats.titleBufferCount[1].load()), bufferKib(stats.titleBufferLargeBytes, 1),
                   bufferKib(stats.titleBufferPageBytes, 1), bufferKib(stats.titleBufferBytes, 0), static_cast<unsigned long long>(stats.titleBufferCount[0].load()),
                   bufferKib(stats.titleBufferLargeBytes, 0), bufferKib(stats.titleBufferPageBytes, 0));
            const auto mib = [&](int bucket) { return static_cast<unsigned long long>(stats.textureBucketBytes[bucket].load() >> 20); };
            const auto count = [&](int bucket) { return static_cast<unsigned long long>(stats.textureBucketCount[bucket].load()); };
            AbxLog("d3d12_x",
                   "texture memory by kind: written targets <256 KiB %llu MiB (%llu), <4 MiB %llu MiB (%llu), larger %llu MiB (%llu) | title textures <256 KiB %llu MiB (%llu), "
                   "<4 MiB %llu MiB (%llu), larger %llu MiB (%llu)",
                   mib(0), count(0), mib(1), count(1), mib(2), count(2), mib(3), count(3), mib(4), count(4), mib(5), count(5));
            AbxLog("d3d12_x",
                   "other video memory AnyBox holds: copy scratch buffers %llu MiB in %llu buffers (%llu MiB of them in system memory), small-texture pool blocks %llu MiB of which "
                   "textures fill %llu MiB | video memory outside AnyBox's textures (pipelines and whatever else the driver keeps there): %lld MiB",
                   static_cast<unsigned long long>(stats.scratchBytes.load() >> 20), static_cast<unsigned long long>(stats.scratchBuffers.load()),
                   static_cast<unsigned long long>(stats.scratchSystemBytes.load() >> 20), static_cast<unsigned long long>(stats.poolBlockBytes.load() >> 20),
                   static_cast<unsigned long long>(stats.poolUsedBytes.load() >> 20),
                   (static_cast<long long>(video.CurrentUsage) - static_cast<long long>(stats.textureBytes.load())) / (1ll << 20));
            ++stats.pipelineMinute;
            AbxLog("d3d12_x",
                   "pipelines the title bound in the last minute: %llu different ones of %llu built so far, %llu of them not bound in the two minutes before (what releasing idle "
                   "pipelines would have to build again)",
                   static_cast<unsigned long long>(stats.pipelinesBoundThisMinute.exchange(0)), static_cast<unsigned long long>(built),
                   static_cast<unsigned long long>(stats.pipelinesBackThisMinute.exchange(0)));
            const WrittenUse use = Resource::MeasureWrittenUse(120);
            const auto part = [](const WrittenUse::Part& value) { return std::pair<unsigned long long, unsigned long long>(value.bytes >> 20, value.count); };
            AbxLog("d3d12_x",
                   "written targets of 4 MiB and more by what command lists did with them: %llu MiB in %llu | never named by a barrier, bind, clear or copy %llu MiB in %llu (of "
                   "these: created in a state that allows writing %llu MiB in %llu, without any view %llu MiB in %llu) | named, but not in the last 120 s %llu MiB in %llu | never "
                   "put in a state that allows writing, bound, cleared or copied into %llu MiB in %llu",
                   part(use.all).first, part(use.all).second, part(use.neverNamed).first, part(use.neverNamed).second, part(use.neverNamedWritable).first,
                   part(use.neverNamedWritable).second, part(use.neverNamedNoView).first, part(use.neverNamedNoView).second, part(use.idle).first, part(use.idle).second,
                   part(use.neverWritten).first, part(use.neverWritten).second);
        }
        DiscordPresenceTick(presents - lastPresents);
        lastPresents = presents;
        lastDraws = draws;
        lastDispatches = dispatches;
        lastSkipped = skipped;
        lastBuilt = built;
        if (console != INVALID_HANDLE_VALUE) {
            const std::string output = std::string(line) + "\r\n";
            DWORD written = 0;
            WriteFile(console, output.data(), static_cast<DWORD>(output.size()), &written, nullptr);
        }
        if (window == nullptr) {
            window = FindProcessWindow();
            if (window != nullptr) {
                wchar_t title[256] = {};
                DWORD_PTR copied = 0;
                if (configuredTitle.empty()) SendMessageTimeoutW(window, WM_GETTEXT, 256, reinterpret_cast<LPARAM>(title), SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &copied);
                title[255] = 0;
                baseTitle = configuredTitle.empty() ? std::wstring(title) : configuredTitle;
                if (FitWindowEnabled()) FitToPrimaryMonitor(window);
                KeepConsoleAside();
                SetForegroundWindow(window);
            }
        }
        if (window != nullptr) {
            const std::wstring text = showStats ? (baseTitle.empty() ? L"" : baseTitle + L" | ") + L"AnyBox " + Widen(line) : baseTitle;
            if (!text.empty() && text != shownTitle) {
                DWORD_PTR result = 0;
                if (SendMessageTimeoutW(window, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()), SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &result) != 0) shownTitle = text;
            }
        }
    }
}

}

LiveStats& Stats() {
    static LiveStats* stats = new LiveStats();
    return *stats;
}

namespace {

std::atomic<long long> g_traceFirst{-1};
std::atomic<long long> g_traceCount{0};

}

bool TracingFrame() {
    static const bool read = [] {
        char value[32] = {};
        GetEnvironmentVariableA("ANYBOX_TRACE_FRAME", value, sizeof(value));
        if (value[0] != 0) {
            g_traceCount.store(3);
            g_traceFirst.store(std::atoll(value));
        }
        return true;
    }();
    static_cast<void>(read);
    const long long first = g_traceFirst.load(std::memory_order_relaxed);
    if (first < 0) return false;
    const long long frame = static_cast<long long>(Stats().presents.load());
    return frame >= first && frame < first + g_traceCount.load(std::memory_order_relaxed);
}

void TraceFrames(std::uint64_t first, std::uint32_t count) {
    TracingFrame();
    g_traceFirst.store(-1);
    g_traceCount.store(static_cast<long long>(count));
    g_traceFirst.store(static_cast<long long>(first));
}

std::uint32_t TextureBucket(bool written, std::uint64_t bytes) {
    const std::uint32_t size = bytes < (256ull << 10) ? 0u : bytes < (4ull << 20) ? 1u : 2u;
    return (written ? 0u : 3u) + size;
}

void StartLiveStats() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!Enabled() && ConfiguredTitle().empty() && !DiscordPresenceConfigured()) return;
        std::thread(Run).detach();
        AbxLog("d3d12_x", "live stats go to the title's window caption and the \"AnyBox stats\" console once a second (d3d12_x.stats = 0 turns them off)");
    });
}

}
