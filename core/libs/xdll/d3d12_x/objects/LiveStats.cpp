#include "LiveStats.hpp"

#include "AbxApi.hpp"
#include "DiscordPresence.hpp"
#include "Presenter.hpp"

#include <windows.h>
#include <dxgi1_4.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

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

void Run() {
    const bool showStats = Enabled();
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
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
        char line[512];
        std::snprintf(line, sizeof(line),
                      "%02lld:%02lld:%02lld | %llu fps | draws %llu/s, dispatches %llu/s, skipped %llu/s | pipelines %llu built (+%llu/s), %llu building, %llu queued, "
                      "%llu failed | textures %llu, %.2f GB | VRAM %.2f of %.2f GB | free commit %.1f GB",
                      static_cast<long long>(seconds / 3600), static_cast<long long>(seconds / 60 % 60), static_cast<long long>(seconds % 60),
                      static_cast<unsigned long long>(presents - lastPresents), static_cast<unsigned long long>(draws - lastDraws),
                      static_cast<unsigned long long>(dispatches - lastDispatches), static_cast<unsigned long long>(skipped - lastSkipped),
                      static_cast<unsigned long long>(built), static_cast<unsigned long long>(built - lastBuilt),
                      static_cast<unsigned long long>(stats.pipelinesBuilding.load()), static_cast<unsigned long long>(stats.pipelinesQueued.load()),
                      static_cast<unsigned long long>(stats.pipelinesFailed.load()), static_cast<unsigned long long>(stats.textureCount.load()),
                      static_cast<double>(stats.textureBytes.load()) / (1ull << 30), static_cast<double>(video.CurrentUsage) / (1ull << 30), static_cast<double>(video.Budget) / (1ull << 30),
                      static_cast<double>(memory.ullAvailPageFile) / (1ull << 30));
        if (showStats && seconds % 10 == 0) AbxLog("d3d12_x", "stats %s", line);
        if (showStats && seconds % 60 == 0) {
            const auto mib = [&](int bucket) { return static_cast<unsigned long long>(stats.textureBucketBytes[bucket].load() >> 20); };
            const auto count = [&](int bucket) { return static_cast<unsigned long long>(stats.textureBucketCount[bucket].load()); };
            AbxLog("d3d12_x",
                   "texture memory by kind: written targets <256 KiB %llu MiB (%llu), <4 MiB %llu MiB (%llu), larger %llu MiB (%llu) | title textures <256 KiB %llu MiB (%llu), "
                   "<4 MiB %llu MiB (%llu), larger %llu MiB (%llu)",
                   mib(0), count(0), mib(1), count(1), mib(2), count(2), mib(3), count(3), mib(4), count(4), mib(5), count(5));
            AbxLog("d3d12_x", "other video memory AnyBox holds: copy scratch buffers %llu MiB in %llu buffers (%llu MiB of them in system memory), small-texture pool blocks %llu MiB",
                   static_cast<unsigned long long>(stats.scratchBytes.load() >> 20), static_cast<unsigned long long>(stats.scratchBuffers.load()),
                   static_cast<unsigned long long>(stats.scratchSystemBytes.load() >> 20), static_cast<unsigned long long>(stats.poolBlockBytes.load() >> 20));
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
                GetWindowTextW(window, title, 256);
                baseTitle = configuredTitle.empty() ? std::wstring(title) : configuredTitle;
                if (FitWindowEnabled()) FitToPrimaryMonitor(window);
                KeepConsoleAside();
                SetForegroundWindow(window);
            }
        }
        if (window != nullptr) {
            const std::wstring text = showStats ? (baseTitle.empty() ? L"" : baseTitle + L" | ") + L"AnyBox " + Widen(line) : baseTitle;
            wchar_t current[512] = {};
            GetWindowTextW(window, current, 512);
            if (!text.empty() && text != current) {
                DWORD_PTR result = 0;
                SendMessageTimeoutW(window, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()), SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &result);
            }
        }
    }
}

}

LiveStats& Stats() {
    static LiveStats* stats = new LiveStats();
    return *stats;
}

bool TracingFrame() {
    static const long long target = [] {
        char value[32] = {};
        GetEnvironmentVariableA("ANYBOX_TRACE_FRAME", value, sizeof(value));
        return value[0] == 0 ? -1ll : std::atoll(value);
    }();
    const long long frame = static_cast<long long>(Stats().presents.load());
    return target >= 0 && frame >= target && frame < target + 3;
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
