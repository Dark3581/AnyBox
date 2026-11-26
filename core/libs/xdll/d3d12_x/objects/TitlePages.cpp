#include "TitlePages.hpp"

#include "AbxApi.hpp"
#include "xdll/xmem/include/GpuWatch.hpp"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace abx::d3d12x {

namespace {

using TrackViews = void (*)(AbxPhysicalPageReturn, void*);
using GuardPage = std::uint32_t (*)(std::uint64_t, BOOL, std::uint64_t*, std::uint32_t);

constexpr auto kPumpPeriod = std::chrono::milliseconds(2000);
constexpr auto kBusyPumpPeriod = std::chrono::milliseconds(250);
constexpr auto kSummaryPeriod = std::chrono::seconds(60);
constexpr std::uint64_t kMaxMebibytes = 8192;
constexpr std::uint64_t kMaxSettleSeconds = 3600;
constexpr unsigned long long kReadDifferencesShown = 16;
constexpr std::uint64_t kRepeatedAccessesShown = 64;

void OnPageGivenOut(void* context, std::uint64_t source) {
    static_cast<TitleAddressSpace*>(context)->ReturnSource(source);
}

bool OnRefusedBuffer(void* context, std::uint64_t address, std::uint64_t bytes, bool write, std::uint64_t caller) {
    return static_cast<TitleAddressSpace*>(context)->ReturnForSystemCall(address, bytes, write, caller);
}

std::uint64_t Number(const char* key, std::uint64_t fallback, std::uint64_t highest) {
    const char* text = AbxSetting(key);
    if (text == nullptr) return fallback;
    char* end = nullptr;
    const std::uint64_t value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || value > highest) {
        AbxUnsupported("D3D12XboxCreateDevice", (std::string(key) + " must be a whole number from 0 to " + std::to_string(highest)).c_str());
    }
    return value;
}

std::string CodeText(std::uint64_t code) {
    if (code == 0) return "";
    HMODULE module = nullptr;
    char path[MAX_PATH] = {};
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(code), &module) || module == nullptr ||
        GetModuleFileNameA(module, path, sizeof(path)) == 0) {
        char text[48];
        std::snprintf(text, sizeof(text), " by code at 0x%llx", static_cast<unsigned long long>(code));
        return text;
    }
    const char* name = std::strrchr(path, '\\');
    char text[MAX_PATH + 48];
    std::snprintf(text, sizeof(text), " by code at %s+0x%llx", name != nullptr ? name + 1 : path,
                  static_cast<unsigned long long>(code - reinterpret_cast<std::uintptr_t>(module)));
    return text;
}

const char* ReasonText(PageReturn reason) {
    switch (reason) {
    case PageReturn::CpuAccess: return "the CPU read or wrote it";
    case PageReturn::UsedByHandle: return "a command named its memory as a buffer (copy, vertex, index or indirect argument buffer, texture upload)";
    case PageReturn::ReadByLayer: return "AnyBox read or wrote it for the title (texture data, readback, memory wait or label, dump)";
    case PageReturn::GivenOut: return "data was streamed into its physical page or the page was mapped at one more address";
    case PageReturn::Remapped: return "its address was given another physical page or its memory was imported again";
    case PageReturn::Unmapped: return "its address or its physical page was released";
    case PageReturn::BufferReleased: return "its buffer was released";
    case PageReturn::GpuMayWrite: return "a buffer or view the GPU may write covers it now";
    case PageReturn::SystemCall: return "a system call of the title was refused its buffer there (ReadFile into it, WriteFile from it)";
    case PageReturn::Unused: return "the GPU had not used it for a minute and a page it uses needed the room";
    case PageReturn::Count: break;
    }
    return "unknown";
}

void LogEvent(const VideoPageEvent& event) {
    const char* const kind = event.own ? " (memory committed on its own)" : "";
    if (event.readOnly && event.pages == 0) {
        AbxLog("d3d12_x",
               "title pages: the read-only copy of the page at 0x%llx%s differed from system memory when it was dropped (%s): the GPU wrote it although no buffer or view said it "
               "may. The GPU's data was kept and the page stays in system memory (%llu such page(s) so far; at %llu no further page is copied)",
               static_cast<unsigned long long>(event.buffer), kind, ReasonText(event.reason), static_cast<unsigned long long>(event.count), kReadDifferencesShown);
        return;
    }
    if (event.readOnly) {
        AbxLog("d3d12_x",
               "title pages: the read-only copy of the page at 0x%llx%s was dropped: %s at 0x%llx%s (%llu such write(s) to this kind of page so far; the first 16 and every power "
               "of two are logged)",
               static_cast<unsigned long long>(event.buffer), kind,
               event.reason == PageReturn::SystemCall ? "a system call of the title was refused its buffer there, called" : "the CPU wrote it",
               static_cast<unsigned long long>(event.address), CodeText(event.code).c_str(), static_cast<unsigned long long>(event.count));
        return;
    }
    if (event.moved) {
        AbxLog("d3d12_x", "title pages: buffer 0x%llx (%llu KiB) got %llu more page(s) in video memory", static_cast<unsigned long long>(event.buffer),
               static_cast<unsigned long long>(event.bufferBytes >> 10), static_cast<unsigned long long>(event.pages));
        return;
    }
    AbxLog("d3d12_x", "title pages: %llu page(s) of buffer 0x%llx (%llu KiB) went back to system memory: %s, at 0x%llx%s%s%s", static_cast<unsigned long long>(event.pages),
           static_cast<unsigned long long>(event.buffer), static_cast<unsigned long long>(event.bufferBytes >> 10), ReasonText(event.reason),
           static_cast<unsigned long long>(event.address), CodeText(event.code).c_str(), event.command != nullptr ? " by " : "", event.command != nullptr ? event.command : "");
}

void Add(VideoPageReport& total, const VideoPageReport& part) {
    total.capBytes = part.capBytes;
    total.heldBytes = part.heldBytes;
    total.heldOwnBytes = part.heldOwnBytes;
    total.arenaBytes = part.arenaBytes;
    total.buffers = part.buffers;
    total.buffersHome = part.buffersHome;
    total.waitingPages = part.waitingPages;
    total.otherPages = part.otherPages;
    total.overCapPages = part.overCapPages;
    total.movedPages += part.movedPages;
    total.movedOwnPages += part.movedOwnPages;
    total.movedSharedPages += part.movedSharedPages;
    total.movedPartialPages += part.movedPartialPages;
    total.lostPages += part.lostPages;
    total.unusedRounds += part.unusedRounds;
    for (std::size_t kind = 0; kind < kPageReturnKinds; ++kind) {
        total.returnedPages[kind] += part.returnedPages[kind];
        total.returnEvents[kind] += part.returnEvents[kind];
    }
    total.sharedPages += part.sharedPages;
    total.pinnedPages += part.pinnedPages;
    total.refusedPages += part.refusedPages;
    total.busyRounds += part.busyRounds;
    total.unsyncedReturns += part.unsyncedReturns;
    total.waitNanoseconds += part.waitNanoseconds;
    total.copyNanoseconds += part.copyNanoseconds;
    total.slowCopyPages += part.slowCopyPages;
    total.droppedEvents += part.droppedEvents;
    total.tableCommands += part.tableCommands;
    total.tableBytes += part.tableBytes;
    total.tableRefused += part.tableRefused;
    total.readCapBytes = part.readCapBytes;
    total.readHeldBytes = part.readHeldBytes;
    total.readKnownPages = part.readKnownPages;
    total.readWaitingPages = part.readWaitingPages;
    total.readStopped = part.readStopped;
    total.readMovedPages += part.readMovedPages;
    for (std::size_t kind = 0; kind < kPageReturnKinds; ++kind) total.readReturnedPages[kind] += part.readReturnedPages[kind];
    total.readWrittenTwice += part.readWrittenTwice;
    total.readRefusedPages += part.readRefusedPages;
    total.readSkippedShared += part.readSkippedShared;
    total.readSkippedWritable += part.readSkippedWritable;
    total.readSkippedPinned += part.readSkippedPinned;
    total.readCheckedPages += part.readCheckedPages;
    total.readDifferedPages += part.readDifferedPages;
    total.readCopyNanoseconds += part.readCopyNanoseconds;
    total.readOwnCapBytes = part.readOwnCapBytes;
    total.readOwnHeldBytes = part.readOwnHeldBytes;
    total.readOwnUsedPages = part.readOwnUsedPages;
    total.readOwnUsedHeldPages = part.readOwnUsedHeldPages;
    total.readOwnRootWrittenPages = part.readOwnRootWrittenPages;
    total.readOwnLookedAt = part.readOwnLookedAt;
    total.readOwnPinnedPages = part.readOwnPinnedPages;
    total.readOwnWritablePages = part.readOwnWritablePages;
    total.readOwnSettlingPages = part.readOwnSettlingPages;
    total.readOwnWaitingPages = part.readOwnWaitingPages;
    total.readOwnMovedPages += part.readOwnMovedPages;
    for (std::size_t kind = 0; kind < kPageReturnKinds; ++kind) total.readOwnReturnedPages[kind] += part.readOwnReturnedPages[kind];
    total.readOwnWrittenTwice += part.readOwnWrittenTwice;
    total.readOwnRefusedPages += part.readOwnRefusedPages;
    total.readOwnUnusedRounds += part.readOwnUnusedRounds;
    total.readOwnCheckedPages += part.readOwnCheckedPages;
    total.readOwnDifferedPages += part.readOwnDifferedPages;
    total.readOwnCopyNanoseconds += part.readOwnCopyNanoseconds;
}

void LogReadSummary(const VideoPageReport& report) {
    const auto back = [&](PageReturn reason) { return static_cast<unsigned long long>(report.readReturnedPages[static_cast<std::size_t>(reason)]); };
    AbxLog("d3d12_x",
           "title pages copied read-only into video memory: %llu KiB of %llu MiB allowed, %llu GPU address(es) of 64 KiB mapped by the title, up to %llu page(s) wait for room%s | last minute: %llu copied in "
           "(%.0f ms); dropped because the CPU wrote them %llu (%llu of them written before as well), a system call of the title was refused its buffer there %llu, data was "
           "streamed into them or they were mapped again %llu, AnyBox read them (texture data, readback, label) %llu, a command named them as a buffer %llu, the GPU may write them now %llu, remapped %llu, unmapped %llu | not copied: %llu "
           "pinned by an earlier use, %llu under a buffer or view the GPU may write, %llu with more than %u GPU addresses, %llu whose views could not be write-protected | "
           "%llu dropped page(s) compared with system memory, %llu differed (the GPU wrote them)",
           static_cast<unsigned long long>(report.readHeldBytes >> 10), static_cast<unsigned long long>(report.readCapBytes >> 20),
           static_cast<unsigned long long>(report.readKnownPages), static_cast<unsigned long long>(report.readWaitingPages),
           report.readStopped ? "; NO FURTHER PAGE IS COPIED, as the GPU wrote pages nothing said it may write" : "", static_cast<unsigned long long>(report.readMovedPages),
           static_cast<double>(report.readCopyNanoseconds) / 1.0e6, back(PageReturn::CpuAccess), static_cast<unsigned long long>(report.readWrittenTwice),
           back(PageReturn::SystemCall), back(PageReturn::GivenOut), back(PageReturn::ReadByLayer), back(PageReturn::UsedByHandle), back(PageReturn::GpuMayWrite), back(PageReturn::Remapped),
           back(PageReturn::Unmapped) + back(PageReturn::BufferReleased), static_cast<unsigned long long>(report.readSkippedPinned),
           static_cast<unsigned long long>(report.readSkippedWritable), static_cast<unsigned long long>(report.readSkippedShared), kVideoPageTitles,
           static_cast<unsigned long long>(report.readRefusedPages), static_cast<unsigned long long>(report.readCheckedPages),
           static_cast<unsigned long long>(report.readDifferedPages));
}

void LogOwnReadSummary(const VideoPageReport& report) {
    const auto back = [&](PageReturn reason) { return static_cast<unsigned long long>(report.readOwnReturnedPages[static_cast<std::size_t>(reason)]); };
    AbxLog("d3d12_x",
           "title memory committed on its own copied read-only into video memory: %llu KiB of %llu MiB allowed%s | GPU use marks: %llu page(s) used in the last minute (%llu of them "
           "with a copy), %llu named by a root unordered access view (never copied) | last minute: %llu copied in (%.0f ms); dropped because the CPU wrote them %llu (%llu of them "
           "written before as well), a system call of the title was refused its buffer there %llu, AnyBox read them or a command named them as a buffer %llu, the GPU may write "
           "them now %llu, unused for a minute and the room needed %llu, released or imported again %llu | last round, of %llu used page(s) without a copy looked at: %llu pinned "
           "by an earlier use, %llu under a buffer or view the GPU may write, %llu in their settle or back-off time, %llu wait for room | not copied in the last minute: %llu not "
           "private read-write memory (could not be write-protected), %llu round(s) in which an import could not be used by the GPU first | %llu dropped page(s) compared with "
           "system memory, %llu differed (the GPU wrote them)",
           static_cast<unsigned long long>(report.readOwnHeldBytes >> 10), static_cast<unsigned long long>(report.readOwnCapBytes >> 20),
           report.readStopped ? "; NO FURTHER PAGE IS COPIED, as the GPU wrote pages nothing said it may write" : "", static_cast<unsigned long long>(report.readOwnUsedPages),
           static_cast<unsigned long long>(report.readOwnUsedHeldPages), static_cast<unsigned long long>(report.readOwnRootWrittenPages),
           static_cast<unsigned long long>(report.readOwnMovedPages), static_cast<double>(report.readOwnCopyNanoseconds) / 1.0e6, back(PageReturn::CpuAccess),
           static_cast<unsigned long long>(report.readOwnWrittenTwice), back(PageReturn::SystemCall), back(PageReturn::ReadByLayer) + back(PageReturn::UsedByHandle),
           back(PageReturn::GpuMayWrite), back(PageReturn::Unused), back(PageReturn::Remapped) + back(PageReturn::Unmapped) + back(PageReturn::BufferReleased),
           static_cast<unsigned long long>(report.readOwnLookedAt), static_cast<unsigned long long>(report.readOwnPinnedPages),
           static_cast<unsigned long long>(report.readOwnWritablePages), static_cast<unsigned long long>(report.readOwnSettlingPages),
           static_cast<unsigned long long>(report.readOwnWaitingPages), static_cast<unsigned long long>(report.readOwnRefusedPages),
           static_cast<unsigned long long>(report.readOwnUnusedRounds), static_cast<unsigned long long>(report.readOwnCheckedPages),
           static_cast<unsigned long long>(report.readOwnDifferedPages));
}

void LogSummary(const VideoPageReport& report) {
    if (report.readCapBytes != 0) LogReadSummary(report);
    if (report.readOwnCapBytes != 0) LogOwnReadSummary(report);
    if (report.capBytes == 0) return;
    const auto pages = [&](PageReturn reason) { return static_cast<unsigned long long>(report.returnedPages[static_cast<std::size_t>(reason)]); };
    const auto times = [&](PageReturn reason) { return static_cast<unsigned long long>(report.returnEvents[static_cast<std::size_t>(reason)]); };
    AbxLog("d3d12_x",
           "title pages in video memory: %llu KiB (%llu KiB of them memory committed on its own) of %llu MiB allowed (%llu MiB of blocks), %llu GPU-writable buffers watched, %llu of "
           "them back in system memory for good | last minute: %llu pages moved in (%llu committed on their own, %llu physical pages with more than one GPU address, the rest physical "
           "pages with one; %llu of them only partly under the buffer); pages back (times): CPU access %llu (%llu), system call %llu (%llu), named as a buffer %llu (%llu), AnyBox access %llu (%llu), streamed or mapped again %llu (%llu), "
           "remapped %llu (%llu), unmapped %llu (%llu), buffer released %llu (%llu)",
           static_cast<unsigned long long>(report.heldBytes >> 10), static_cast<unsigned long long>(report.heldOwnBytes >> 10), static_cast<unsigned long long>(report.capBytes >> 20),
           static_cast<unsigned long long>(report.arenaBytes >> 20), static_cast<unsigned long long>(report.buffers), static_cast<unsigned long long>(report.buffersHome),
           static_cast<unsigned long long>(report.movedPages), static_cast<unsigned long long>(report.movedOwnPages), static_cast<unsigned long long>(report.movedSharedPages),
           static_cast<unsigned long long>(report.movedPartialPages), pages(PageReturn::CpuAccess), times(PageReturn::CpuAccess), pages(PageReturn::SystemCall), times(PageReturn::SystemCall), pages(PageReturn::UsedByHandle), times(PageReturn::UsedByHandle), pages(PageReturn::ReadByLayer),
           times(PageReturn::ReadByLayer), pages(PageReturn::GivenOut), times(PageReturn::GivenOut), pages(PageReturn::Remapped), times(PageReturn::Remapped),
           pages(PageReturn::Unmapped), times(PageReturn::Unmapped), pages(PageReturn::BufferReleased), times(PageReturn::BufferReleased));
    AbxLog("d3d12_x",
           "title pages left in system memory: %llu have no memory at their address yet, %llu are in memory that is neither a physical page nor committed on its own, %llu do not "
           "fit the allowance now | decided in the last minute: %llu have a physical page with more than %u GPU addresses, %llu are pinned by an earlier use, %llu could not be "
           "guarded (views, or memory that is not private read-write or read-only) | waiting for the GPU %.0f ms, copying %.0f ms (%llu page(s) read back through the CPU mapping, "
           "the slow way), %llu round(s) given up on a busy GPU, %llu round(s) in which a new import could not be used by the GPU first, %llu return(s) without an idle GPU, %llu "
           "page(s) whose memory was gone at the return, %llu event line(s) dropped | %llu copy, fill or indirect argument command(s) ran through the page table (%llu KiB), %llu "
           "named memory the GPU may write as a buffer although the setting is on (not 4-byte aligned, over 256 MiB, overlapping, or not all in mapped memory)",
           static_cast<unsigned long long>(report.waitingPages), static_cast<unsigned long long>(report.otherPages), static_cast<unsigned long long>(report.overCapPages),
           static_cast<unsigned long long>(report.sharedPages), kVideoPageTitles, static_cast<unsigned long long>(report.pinnedPages),
           static_cast<unsigned long long>(report.refusedPages), static_cast<double>(report.waitNanoseconds) / 1.0e6, static_cast<double>(report.copyNanoseconds) / 1.0e6,
           static_cast<unsigned long long>(report.slowCopyPages), static_cast<unsigned long long>(report.busyRounds), static_cast<unsigned long long>(report.unusedRounds),
           static_cast<unsigned long long>(report.unsyncedReturns),
           static_cast<unsigned long long>(report.lostPages), static_cast<unsigned long long>(report.droppedEvents),
           static_cast<unsigned long long>(report.tableCommands), static_cast<unsigned long long>(report.tableBytes >> 10),
           static_cast<unsigned long long>(report.tableRefused));
}

}

std::unique_ptr<TitlePages> TitlePages::Start(TitleAddressSpace& space) {
    const std::uint64_t mebibytes = Number("d3d12_x.title_pages_in_vram_mib", 0, kMaxMebibytes);
    const std::uint64_t readMebibytes = Number("d3d12_x.title_pages_read_only_mib", 0, kMaxMebibytes);
    const std::uint64_t ownMebibytes = Number("d3d12_x.title_pages_read_only_private_mib", 0, kMaxMebibytes);
    if (mebibytes == 0) {
        AbxLog("d3d12_x", "pages of buffers the GPU may write stay in system memory, which shaders reach over the bus (d3d12_x.title_pages_in_vram_mib = <MiB> keeps them in video "
                          "memory until the CPU touches them)");
    }
    if (readMebibytes == 0) {
        AbxLog("d3d12_x", "physical pages the title maps for the GPU and only the CPU writes (geometry that shaders read through raw addresses) stay in system memory "
                          "(d3d12_x.title_pages_read_only_mib = <MiB> keeps read-only copies of them in video memory until the CPU writes them)");
    }
    if (ownMebibytes == 0) {
        AbxLog("d3d12_x", "memory committed on its own that shaders read through buffer views or raw addresses stays in system memory "
                          "(d3d12_x.title_pages_read_only_private_mib = <MiB> keeps read-only copies of the pages the GPU used in the last minute in video memory until the CPU "
                          "writes them)");
    }
    if (mebibytes == 0 && readMebibytes == 0 && ownMebibytes == 0) return nullptr;
    VideoPageSetup setup;
    setup.capBytes = mebibytes << 20;
    setup.settleSeconds = static_cast<std::uint32_t>(Number("d3d12_x.title_pages_settle_seconds", 5, kMaxSettleSeconds));
    const char* unit = AbxSetting("d3d12_x.title_pages_return");
    if (unit != nullptr && std::strcmp(unit, "buffer") != 0 && std::strcmp(unit, "page") != 0) {
        AbxUnsupported("D3D12XboxCreateDevice", "d3d12_x.title_pages_return must be buffer or page");
    }
    setup.wholeBuffer = unit == nullptr || std::strcmp(unit, "buffer") == 0;
    setup.partialPages = Number("d3d12_x.title_pages_partial", 0, 1) != 0;
    setup.tableCommands = Number("d3d12_x.title_pages_table_commands", 0, 1) != 0;
    if (setup.tableCommands && mebibytes == 0) {
        AbxLog("d3d12_x", "d3d12_x.title_pages_table_commands = 1 has no effect without d3d12_x.title_pages_in_vram_mib: no page of a buffer the GPU may write moves");
    }
    setup.readCapBytes = readMebibytes << 20;
    setup.readSettleSeconds = static_cast<std::uint32_t>(Number("d3d12_x.title_pages_read_only_settle_seconds", 10, kMaxSettleSeconds));
    setup.readOwnCapBytes = ownMebibytes << 20;
    const HMODULE xmem = GetModuleHandleW(L"ab_xmem.dll");
    const auto track = xmem != nullptr ? reinterpret_cast<TrackViews>(reinterpret_cast<void*>(GetProcAddress(xmem, "AbxXMemTrackPhysicalViews"))) : nullptr;
    const auto guard = xmem != nullptr ? reinterpret_cast<GuardPage>(reinterpret_cast<void*>(GetProcAddress(xmem, "AbxXMemGuardPhysicalPage"))) : nullptr;
    if (track == nullptr || guard == nullptr) {
        AbxLog("d3d12_x", "a d3d12_x.title_pages_* allowance is set, but ab_xmem.dll does not export AbxXMemTrackPhysicalViews and AbxXMemGuardPhysicalPage: title pages stay in "
                          "system memory");
        return nullptr;
    }
    setup.guard = guard;
    std::string error;
    if (!space.EnableVideoPages(setup, error)) {
        AbxLog("d3d12_x", "a d3d12_x.title_pages_* allowance is set, but title pages stay in system memory: %s", error.c_str());
        return nullptr;
    }
    track(&OnPageGivenOut, &space);
    const char* const systemCalls =
        AbxWatchRefusedBuffers(&OnRefusedBuffer, &space)
            ? "A title module's ReadFile or WriteFile that the system refuses at the call because of such a page (ERROR_NOACCESS or ERROR_INVALID_USER_BUFFER) gets the pages "
              "under its buffer back and is repeated; a WriteFile on an overlapped handle, which fails later, and any other system call that reads or writes such a page "
              "directly still fail"
            : "Not covered: a system call that reads or writes such a page directly (ReadFile into it) fails";
    if (readMebibytes != 0) {
        AbxLog("d3d12_x",
               "physical pages the title maps at GPU addresses (command queue slot 31) get a read-only copy in video memory, up to %llu MiB (d3d12_x.title_pages_read_only_mib): a "
               "page mapped so for %u s (d3d12_x.title_pages_read_only_settle_seconds) that has at most %u GPU addresses, lies under no buffer or view the GPU may write and was not read by "
               "AnyBox as texture data or named as a buffer. Shaders read the copy through the page table; every CPU view stays readable and loses write access, and the first "
               "CPU write, streamed write or new mapping drops the copy at once (no wait for the GPU) and the page is copied again after twice the time. Dropped copies are "
               "compared with system memory (the first 64, then every 8th written and every 64th other one, all of them once one differed): a difference means the GPU wrote "
               "the page, its data is kept, and after 16 such pages no further page is copied. %s; memory committed on its own has a setting of its own (d3d12_x.title_pages_read_only_private_mib)",
               static_cast<unsigned long long>(readMebibytes), setup.readSettleSeconds, kVideoPageTitles, systemCalls);
    }
    if (ownMebibytes != 0) {
        AbxLog("d3d12_x",
               "memory committed on its own gets read-only copies in video memory, up to %llu MiB (d3d12_x.title_pages_read_only_private_mib): a 64 KiB page of private read-write "
               "or read-only memory imported on its own that a buffer view, a root view or an address pair in the root words of a draw or dispatch named in the last minute "
               "(marks age in steps of %llu s), for %u s (d3d12_x.title_pages_read_only_settle_seconds), that lies under no buffer or view the GPU may write and was not read by "
               "AnyBox or named as a buffer. The page is made read-only and copied, and shaders read the copy through the page table; the first CPU write or refused system call "
               "drops the copy at once (no wait for the GPU) and the page is copied again after 2 to 16 times the settle time; pages the GPU stopped using give their room to "
               "used ones. A root unordered access view marks %llu KiB from its address as written by the GPU. Dropped copies are compared with system memory as for physical "
               "pages. %s",
               static_cast<unsigned long long>(ownMebibytes), static_cast<unsigned long long>(kUseMarkEpochSeconds), setup.readSettleSeconds, static_cast<unsigned long long>(kRootWriteMarkBytes >> 10),
               systemCalls);
    }
    if (mebibytes != 0) {
        AbxLog("d3d12_x",
               "pages of buffers the GPU may write move into video memory, up to %llu MiB (d3d12_x.title_pages_in_vram_mib): whole 64 KiB pages of such a buffer that is %u s old "
               "(d3d12_x.title_pages_settle_seconds), whose memory is a physical page with at most %u GPU addresses or private memory committed on its own; every CPU view of the "
               "page loses access, and the first CPU access, buffer-handle use, AnyBox access or streamed write copies %s back and leaves it in system memory "
               "(d3d12_x.title_pages_return = buffer | page). %s. %s. %s",
               static_cast<unsigned long long>(mebibytes), setup.settleSeconds, kVideoPageTitles, setup.wholeBuffer ? "the whole buffer" : "that page", systemCalls,
               setup.tableCommands ? "Copies and fills by title address or between buffers, unordered access view clears of buffers and the argument copy of indirect "
                                     "dispatches and mesh draws run as compute work that reaches the memory through the page table when a page of it lies under a "
                                     "buffer or view the GPU may write, so they do not bring such a page back (d3d12_x.title_pages_table_commands = 1); vertex and "
                                     "index buffers, indirect draw arguments, indirect counts and texture copies still name the memory as a buffer"
                                   : "A copy, fill, unordered access view clear or indirect argument buffer inside such a page names the memory as a buffer and brings "
                                     "the page back for good (d3d12_x.title_pages_table_commands = 1 runs them through the page table instead)",
               setup.partialPages ? "The first and last page of such a buffer move too when the buffer covers only part of them (d3d12_x.title_pages_partial = 1): the "
                                    "rest of such a page is guarded with it, so a CPU access to other data there brings the buffer back"
                                  : "A page the buffer covers only in part stays in system memory (d3d12_x.title_pages_partial = 1 moves it too)");
    }
    std::unique_ptr<TitlePages> pages(new TitlePages(space));
    pages->thread_ = std::thread([raw = pages.get()] { raw->Run(); });
    return pages;
}

TitlePages::~TitlePages() {
    AbxWatchRefusedBuffers(nullptr, nullptr);
    {
        const std::lock_guard guard(lock_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void TitlePages::Run() {
    VideoPageReport minute;
    std::uint64_t repeatedShown = 0;
    auto summary = std::chrono::steady_clock::now() + kSummaryPeriod;
    bool more = false;
    for (;;) {
        {
            std::unique_lock guard(lock_);
            if (wake_.wait_for(guard, more ? kBusyPumpPeriod : kPumpPeriod, [&] { return stop_; })) return;
        }
        more = space_.PumpVideoPages();
        const VideoPageReport part = space_.TakeVideoPageReport();
        for (const VideoPageEvent& event : part.events) LogEvent(event);
        std::uint64_t repeatedAddress = 0;
        std::uint64_t repeatedCode = 0;
        std::uint32_t repeatedThread = 0;
        const std::uint64_t repeated = TitleAddressSpace::RepeatedAccesses(repeatedAddress, repeatedCode, repeatedThread);
        if (repeated >= kRepeatedAccessesShown && repeated >= 2 * repeatedShown) {
            repeatedShown = repeated;
            AbxLog("d3d12_x",
                   "title pages: thread %lu was resumed %llu times in a row after the same access to a guarded page at 0x%llx%s and faulted there again each time: the "
                   "page it needs does not become accessible, so that thread makes no progress (logged again when the count has doubled)",
                   static_cast<unsigned long>(repeatedThread), static_cast<unsigned long long>(repeated), static_cast<unsigned long long>(repeatedAddress),
                   CodeText(repeatedCode).c_str());
        }
        Add(minute, part);
        if (std::chrono::steady_clock::now() < summary) continue;
        summary += kSummaryPeriod;
        LogSummary(minute);
        minute = VideoPageReport{};
    }
}

}
