#pragma once

#include "VulkanDevice.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace abx::d3d12x {

constexpr std::uint64_t kTitlePageBytes = 64ull << 10;
constexpr std::uint64_t kTitleRegionBytes = 4ull << 30;
constexpr std::uint64_t kTitleRegionCount = 1ull << 15;
constexpr std::uint64_t kTitlePagesPerRegion = kTitleRegionBytes / kTitlePageBytes;

constexpr std::uint32_t kVideoPageViews = 8;
constexpr std::uint32_t kVideoPageTitles = 4;
constexpr std::uint32_t kVideoPageGuardRefused = 0xFFFFFFFFu;
constexpr int kVideoPageGuardAll = 1;
constexpr int kVideoPageGuardWrites = 2;
constexpr std::uint32_t kReadPageBackoffSteps = 4;
constexpr std::uint32_t kUseMarkEpochSeconds = 15;
constexpr std::uint64_t kRootWriteMarkBytes = 1ull << 20;

enum class PageReturn : std::uint32_t { CpuAccess, UsedByHandle, ReadByLayer, GivenOut, Remapped, Unmapped, BufferReleased, GpuMayWrite, SystemCall, Unused, Count };

constexpr std::size_t kPageReturnKinds = static_cast<std::size_t>(PageReturn::Count);

using VideoPageGuard = std::uint32_t (*)(std::uint64_t source, int guard, std::uint64_t* views, std::uint32_t capacity);

struct VideoPageSetup {
    std::uint64_t capBytes = 0;
    std::uint32_t settleSeconds = 5;
    bool wholeBuffer = true;
    bool partialPages = false;
    bool tableCommands = false;
    VideoPageGuard guard = nullptr;
    std::uint64_t readCapBytes = 0;
    std::uint32_t readSettleSeconds = 10;
    std::uint64_t readOwnCapBytes = 0;
    std::uint32_t useMarkEpochMilliseconds = kUseMarkEpochSeconds * 1000;
};

struct VideoPageEvent {
    bool moved = false;
    PageReturn reason = PageReturn::CpuAccess;
    std::uint64_t buffer = 0;
    std::uint64_t bufferBytes = 0;
    std::uint64_t pages = 0;
    std::uint64_t address = 0;
    std::uint64_t code = 0;
    bool readOnly = false;
    std::uint64_t count = 0;
    bool own = false;
    const char* command = nullptr;
};

struct TitleAccess {
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
    bool write = false;
};

struct VideoPageReport {
    std::uint64_t capBytes = 0;
    std::uint64_t heldBytes = 0;
    std::uint64_t heldOwnBytes = 0;
    std::uint64_t arenaBytes = 0;
    std::uint64_t buffers = 0;
    std::uint64_t buffersHome = 0;
    std::uint64_t movedPages = 0;
    std::uint64_t movedOwnPages = 0;
    std::uint64_t movedSharedPages = 0;
    std::uint64_t movedPartialPages = 0;
    std::uint64_t returnedPages[kPageReturnKinds] = {};
    std::uint64_t returnEvents[kPageReturnKinds] = {};
    std::uint64_t waitingPages = 0;
    std::uint64_t otherPages = 0;
    std::uint64_t sharedPages = 0;
    std::uint64_t pinnedPages = 0;
    std::uint64_t refusedPages = 0;
    std::uint64_t overCapPages = 0;
    std::uint64_t busyRounds = 0;
    std::uint64_t unsyncedReturns = 0;
    std::uint64_t waitNanoseconds = 0;
    std::uint64_t copyNanoseconds = 0;
    std::uint64_t slowCopyPages = 0;
    std::uint64_t lostPages = 0;
    std::uint64_t unusedRounds = 0;
    std::uint64_t droppedEvents = 0;
    std::uint64_t tableCommands = 0;
    std::uint64_t tableBytes = 0;
    std::uint64_t tableRefused = 0;
    std::uint64_t readCapBytes = 0;
    std::uint64_t readHeldBytes = 0;
    std::uint64_t readKnownPages = 0;
    std::uint64_t readMovedPages = 0;
    std::uint64_t readReturnedPages[kPageReturnKinds] = {};
    std::uint64_t readWrittenTwice = 0;
    std::uint64_t readWaitingPages = 0;
    std::uint64_t readRefusedPages = 0;
    bool readStopped = false;
    std::uint64_t readSkippedShared = 0;
    std::uint64_t readSkippedWritable = 0;
    std::uint64_t readSkippedPinned = 0;
    std::uint64_t readCheckedPages = 0;
    std::uint64_t readDifferedPages = 0;
    std::uint64_t readCopyNanoseconds = 0;
    std::uint64_t readOwnCapBytes = 0;
    std::uint64_t readOwnHeldBytes = 0;
    std::uint64_t readOwnUsedPages = 0;
    std::uint64_t readOwnUsedHeldPages = 0;
    std::uint64_t readOwnRootWrittenPages = 0;
    std::uint64_t readOwnLookedAt = 0;
    std::uint64_t readOwnPinnedPages = 0;
    std::uint64_t readOwnWritablePages = 0;
    std::uint64_t readOwnSettlingPages = 0;
    std::uint64_t readOwnWaitingPages = 0;
    std::uint64_t readOwnMovedPages = 0;
    std::uint64_t readOwnReturnedPages[kPageReturnKinds] = {};
    std::uint64_t readOwnWrittenTwice = 0;
    std::uint64_t readOwnRefusedPages = 0;
    std::uint64_t readOwnUnusedRounds = 0;
    std::uint64_t readOwnCheckedPages = 0;
    std::uint64_t readOwnDifferedPages = 0;
    std::uint64_t readOwnCopyNanoseconds = 0;
    std::vector<VideoPageEvent> events;
};

class TitleAddressSpace {
public:
    static std::unique_ptr<TitleAddressSpace> Create(const VulkanDevice& device, std::string& error);
    ~TitleAddressSpace();
    TitleAddressSpace(const TitleAddressSpace&) = delete;
    TitleAddressSpace& operator=(const TitleAddressSpace&) = delete;

    VkDeviceAddress RootTable() const { return root_.address; }
    bool Import(std::uint64_t start, std::uint64_t bytes, std::string& error);
    bool MapDevice(std::uint64_t start, std::uint64_t bytes, VkDeviceAddress device, VkBuffer buffer, std::string& error);
    void Release(std::uint64_t start, std::uint64_t bytes);
    void AddBacking(std::uint64_t view, std::uint64_t bytes);
    void RemoveBacking(std::uint64_t view, std::uint64_t bytes);
    bool AliasPhysical(std::uint64_t start, std::uint64_t bytes, std::uint64_t source, std::string& error, bool gpuMapped = false);
    const std::uint8_t* HostAt(std::uint64_t address) const;
    bool Contains(std::uint64_t address, std::uint64_t bytes) const;
    struct Location {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
    };
    std::optional<Location> Locate(std::uint64_t address, std::uint64_t bytes) const;
    std::optional<Location> LocateRun(std::uint64_t address, std::uint64_t& available, std::uint64_t wanted = ~std::uint64_t{0}) const;
    std::uint64_t ImportedBytes() const;
    std::string DescribeDeviceAddress(std::uint64_t device) const;
    std::string DescribeTitleAddress(std::uint64_t address) const;
    std::uint64_t ReadForTrace(std::uint64_t address, void* destination, std::uint64_t bytes) const;
    std::string DescribePlacement(std::uint64_t address, std::uint64_t bytes) const;
    std::string DescribeBacking(std::uint64_t address, std::uint64_t bytes) const;
    std::optional<std::uint64_t> AliasBelow4GiB(std::uint64_t start, std::uint64_t bytes, std::string& error);
    void ReleaseAlias(std::uint64_t alias);

    bool EnableVideoPages(const VideoPageSetup& setup, std::string& error);
    void WatchBuffer(std::uint64_t address, std::uint64_t bytes, bool added);
    void MarkGpuWritten(std::uint64_t address, std::uint64_t bytes);
    bool TableCommands() const { return tableCommands_.load(std::memory_order_relaxed); }
    bool TableCommand(const TitleAccess* accesses, std::size_t count);
    class Naming {
    public:
        explicit Naming(const char* command);
        ~Naming();
        Naming(const Naming&) = delete;
        Naming& operator=(const Naming&) = delete;

    private:
        const char* previous_ = nullptr;
    };
    void MarkGpuRead(std::uint64_t address, std::uint64_t bytes);
    void MarkGpuRootWords(const std::uint32_t* words, std::uint32_t count);
    void MarkGpuRootWrite(std::uint64_t address);
    bool PumpVideoPages();
    VideoPageReport TakeVideoPageReport();
    void ReturnSource(std::uint64_t source);
    bool ReturnForAccess(std::uint64_t address, std::uint64_t code, bool write);
    bool ReturnForSystemCall(std::uint64_t address, std::uint64_t bytes, bool write, std::uint64_t code);
    std::uint64_t TableEntry(std::uint64_t address) const;
    std::vector<std::string> DescribeActivity(std::uint32_t lockProbeMilliseconds, std::uint32_t compareMilliseconds) const;
    std::string CompareReadCopies(std::uint32_t lockProbeMilliseconds, std::uint32_t limitMilliseconds) const;
    static std::uint64_t RepeatedAccesses(std::uint64_t& address, std::uint64_t& code, std::uint32_t& thread);

private:
    struct Allocation {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceAddress address = 0;
        void* mapped = nullptr;
    };
    struct Range {
        std::uint64_t bytes = 0;
        VkDeviceAddress device = 0;
        Allocation imported;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize bufferOffset = 0;
        bool named = false;
        bool used = false;
    };
    struct VideoBlock {
        Allocation allocation;
        std::uint64_t bytes = 0;
        std::vector<std::uint32_t> free;
    };
    struct MovedPage {
        std::uint64_t title = 0;
        std::uint32_t block = 0;
        std::uint32_t page = 0;
        std::uint64_t group = 0;
        std::uint32_t viewCount = 0;
        std::uint64_t views[kVideoPageViews] = {};
        std::uint32_t extraCount = 0;
        std::uint64_t extra[kVideoPageTitles - 1] = {};
        bool own = false;
        std::uint32_t protection = 0;
        bool readOnly = false;
    };
    using UseMarks = std::atomic<std::uint8_t>;
    struct OwnSnapshot {
        std::uint64_t used = 0;
        std::uint64_t usedHeld = 0;
        std::uint64_t rootWritten = 0;
        std::uint64_t lookedAt = 0;
        std::uint64_t pinned = 0;
        std::uint64_t writable = 0;
        std::uint64_t settling = 0;
        std::uint64_t waiting = 0;
    };
    struct ReadState {
        std::chrono::steady_clock::time_point due;
        std::uint32_t writes = 0;
    };
    struct FreedPage {
        std::uint32_t block = 0;
        std::uint32_t page = 0;
    };
    struct WatchedBuffer {
        std::uint64_t bytes = 0;
        std::uint64_t id = 0;
        std::chrono::steady_clock::time_point added;
        bool home = false;
        bool complete = false;
    };
    using WatchedBuffers = std::multimap<std::uint64_t, WatchedBuffer>;
    struct Candidate {
        std::uint64_t title = 0;
        std::uint64_t source = 0;
        std::uint64_t group = 0;
        bool own = false;
        bool partial = false;
    };
    struct SettledBuffer {
        std::uint64_t id = 0;
        std::uint64_t shared = 0;
        std::uint64_t pinned = 0;
    };
    struct VideoPages {
        VideoPageSetup setup;
        std::vector<VideoBlock> blocks;
        std::uint64_t arenaBytes = 0;
        std::uint64_t heldBytes = 0;
        std::uint64_t heldOwnBytes = 0;
        std::map<std::uint64_t, std::uint64_t> ownPinned;
        std::vector<std::uint8_t> staging;
        std::map<std::uint64_t, MovedPage> moved;
        std::unordered_map<std::uint64_t, VkDeviceAddress> movedTitles;
        std::unordered_map<std::uint64_t, std::uint64_t> guardedViews;
        std::unordered_set<std::uint64_t> everGuarded;
        std::set<std::uint64_t> pinned;
        std::unordered_multimap<std::uint64_t, std::uint64_t> titlesOf;
        WatchedBuffers buffers;
        std::unordered_map<std::uint64_t, WatchedBuffers::iterator> byId;
        std::uint64_t nextId = 1;
        std::chrono::steady_clock::time_point nextPump;
        std::vector<VkFence> lateFences;
        bool scratchTaken = false;
        std::uint32_t scratchBlock = 0;
        std::uint32_t scratchPage = 0;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        std::uint64_t arenaCap = 0;
        std::uint64_t heldReadBytes = 0;
        std::unordered_map<std::uint64_t, ReadState> readStates;
        std::multimap<std::chrono::steady_clock::time_point, std::uint64_t> readDue;
        std::map<std::uint64_t, std::uint64_t> gpuWritten;
        std::unordered_set<std::uint64_t> gpuMapped;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> watchedSpans;
        bool watchedSpansStale = true;
        std::vector<FreedPage> freed;
        std::vector<FreedPage> freedBehindFence;
        VkFence freedFence = VK_NULL_HANDLE;
        std::uint64_t readDrops = 0;
        std::uint64_t readWriteEvents = 0;
        std::uint64_t readDiffered = 0;
        bool readStopped = false;
        std::uint64_t readWaiting = 0;
        std::chrono::steady_clock::time_point readWaitingAt;
        std::uint64_t heldOwnReadBytes = 0;
        std::map<std::uint64_t, ReadState> ownStates;
        std::vector<std::uint64_t> markRegions;
        std::vector<std::unique_ptr<UseMarks[]>> markArrays;
        std::uint64_t ownCursor = 0;
        std::uint64_t ownWriteEvents = 0;
        OwnSnapshot ownSnapshot;
        VkCommandPool usePool = VK_NULL_HANDLE;
        VkCommandBuffer useCommands = VK_NULL_HANDLE;
        VkFence useFence = VK_NULL_HANDLE;
        std::vector<std::pair<std::uint64_t, VkDeviceMemory>> usePending;
        VideoPageReport report;
    };

    std::map<std::uint64_t, std::uint64_t> hostPages_;
    explicit TitleAddressSpace(const VulkanDevice& device) : device_(device) {}
    bool Initialize(std::string& error);
    bool CreateTable(std::uint64_t bytes, Allocation& table, std::string& error);
    bool ImportLocked(std::uint64_t start, std::uint64_t bytes, std::string& error);
    bool ImportHost(std::uint64_t start, std::uint64_t bytes, Allocation& imported, std::string& error);
    std::map<std::uint64_t, Range>::iterator FirstOverlap(std::uint64_t start);
    const Allocation* BackingChunk(std::uint64_t source, std::uint64_t& chunkStart, std::uint64_t& chunkBytes, std::string& error);
    void Destroy(Allocation& allocation);
    std::uint64_t* RegionTable(std::uint64_t region, std::string& error);
    void Point(std::uint64_t start, std::uint64_t bytes, VkDeviceAddress device, std::uint64_t deviceStart);
    void Clear(std::uint64_t start, std::uint64_t bytes);
    void PointAliases();
    bool CreateVideoBlock(std::uint64_t bytes, Allocation& block, std::string& error);
    bool TakeVideoPage(std::uint32_t& block, std::uint32_t& page);
    std::uint64_t VideoRoom() const;
    std::uint64_t ArenaRoom() const;
    std::uint64_t ReadRoom() const;
    std::uint64_t ReadOwnRoom() const;
    bool PumpReadPages();
    bool PumpOwnReadPages();
    bool MoveOwnReadPage(std::uint64_t title);
    int OwnReadCandidate(std::uint64_t title, std::chrono::steady_clock::time_point now);
    void AddUseMarks(std::uint64_t start, std::uint64_t bytes);
    UseMarks* MarkAt(std::uint64_t title) const;
    void SetHeldMark(std::uint64_t title, bool held);
    std::uint8_t UseEpoch() const;
    bool PumpWritablePages();
    void CollectFreedPages();
    void DeferRead(std::uint64_t source, std::chrono::steady_clock::time_point due);
    bool GpuMayWrite(std::uint64_t title);
    bool Covered(std::uint64_t address, std::uint64_t bytes) const;
    void MarkWritten(std::uint64_t address, std::uint64_t bytes);
    int ReadCandidate(std::uint64_t source);
    bool MoveReadPage(std::uint64_t source);
    void DropReadPage(std::uint64_t source, PageReturn reason, std::uint64_t address, std::uint64_t code);
    void DropReadSpan(std::uint64_t start, std::uint64_t end);
    bool WaitForQueue(std::uint64_t timeoutNanoseconds);
    void SetEntry(std::uint64_t title, std::uint64_t entry);
    std::uint64_t HostEntry(std::uint64_t title) const;
    bool Movable(const Candidate& candidate) const;
    void CollectVideoWork(std::vector<Candidate>& candidates, std::vector<std::uint64_t>& leaving, std::vector<SettledBuffer>& settled);
    bool CopyPagesOut(const std::vector<std::uint64_t>& sources);
    void ReturnPagesHeld(const std::vector<std::uint64_t>& sources, PageReturn reason, bool idle);
    void ReturnPages(const std::vector<std::uint64_t>& sources, PageReturn reason);
    void ReturnForUse(std::uint64_t source, PageReturn reason, std::uint64_t address, std::uint64_t code);
    void PinSource(std::uint64_t source, PageReturn reason, std::uint64_t address);
    void PinRange(std::map<std::uint64_t, Range>::iterator range, std::uint64_t address, std::uint64_t bytes);
    void PinSpan(std::uint64_t start, std::uint64_t bytes);
    void PinOwn(std::uint64_t start, std::uint64_t bytes, PageReturn reason, std::uint64_t address);
    bool OwnPinned(std::uint64_t title) const;
    void CutOwnPins(std::uint64_t start, std::uint64_t end);
    bool OwnRange(std::uint64_t title) const;
    void ReturnOwnSpan(std::uint64_t start, std::uint64_t end, PageReturn reason);
    void Reconsider(const std::vector<std::uint64_t>& sources);
    bool GuardOwnPage(std::uint64_t title, void* copy, std::uint32_t& protection);
    bool UseOwnRanges(const std::vector<Candidate>& candidates);
    bool UseOwnRangesLater(const std::vector<std::uint64_t>& starts);
    void NoteAlias(std::uint64_t start, std::uint64_t bytes, std::uint64_t source, bool gpuMapped);
    void NoteRelease(std::uint64_t start, std::uint64_t end);
    void NoteBackingRemoved(std::uint64_t view, std::uint64_t bytes);
    void ForgetTitle(std::uint64_t source, std::uint64_t title);
    void AddEvent(const VideoPageEvent& event);
    struct Alias {
        std::uint64_t start = 0;
        std::uint64_t bytes = 0;
        std::uint64_t slot = 0;
    };

    const VulkanDevice& device_;
    mutable std::mutex lock_;
    Allocation root_;
    Allocation nullRegion_;
    Allocation nullPage_;
    std::vector<Allocation> regions_;
    std::map<std::uint64_t, Range> ranges_;
    std::uint64_t importedBytes_ = 0;
    void* aliasReservation_ = nullptr;
    std::uint64_t aliasBase_ = 0;
    std::uint64_t aliasBytes_ = 0;
    std::map<std::uint64_t, Alias> aliases_;
    std::map<std::uint64_t, std::uint64_t> backings_;
    std::map<std::uint64_t, Allocation> chunks_;
    std::unique_ptr<VideoPages> video_;
    std::atomic<bool> everMoved_{false};
    std::atomic<bool> tableCommands_{false};
    std::unique_ptr<std::atomic<UseMarks*>[]> useMarks_;
    std::atomic<bool> marking_{false};
    std::atomic<std::uint32_t> epochMilliseconds_{kUseMarkEpochSeconds * 1000};
};

}
