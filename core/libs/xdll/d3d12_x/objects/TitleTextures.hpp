#pragma once

#include "Gfx10Tiling.hpp"
#include "CommandRecorder.hpp"
#include "Resource.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <optional>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace abx::d3d12x {

class TitleTextures {
public:
    TitleTextures(const ResourceContext& context, IUnknown* parent);
    static TitleTextures* Active() { return active_.load(); }
    ~TitleTextures();
    TitleTextures(const TitleTextures&) = delete;
    TitleTextures& operator=(const TitleTextures&) = delete;

    Resource* At(std::uint64_t address, const D3D12_RESOURCE_DESC& desc, std::string& error);
    void Written(std::uint64_t start, std::uint64_t bytes);
    void Refresh();
    void Flush();
    void Report();
    std::string Describe(Resource* texture);
    void FillFloats(std::uint32_t width, std::uint32_t height, float value, bool on);

private:
    enum class Filled { Yes, NotMapped, Failed };
    Filled Upload(Resource& texture, std::uint64_t address, std::uint32_t mips, std::string& error, bool filledBefore = false);
    bool MakeReadable(Resource& texture, std::string& error);

    void RefreshNow();
    void RunRefreshes();

    ResourceContext context_;
    IUnknown* parent_;
    std::mutex lock_;
    bool ownThread_ = false;
    std::size_t maxInFlight_ = 8;
    std::thread thread_;
    std::mutex wakeLock_;
    std::condition_variable wake_;
    bool due_ = false;
    bool stop_ = false;
    std::map<std::pair<std::uint64_t, std::vector<std::uint8_t>>, Resource*> textures_;
    std::map<Resource*, std::pair<std::uint64_t, std::uint32_t>> pending_;
    struct Tracked {
        std::uint64_t address = 0;
        std::uint64_t bytes = 0;
        std::uint32_t mips = 0;
        std::uint64_t sample = 0;
        bool readable = false;
        bool whole = false;
        bool direct = true;
        std::uint64_t lastUsed = 0;
        bool settling = false;
        std::uint64_t candidate = 0;
        std::uint32_t changedChecks = 0;
        std::uint64_t uploads = 0;
        std::uint64_t lastUpload = 0;
        std::uint64_t fillGeneration = 0;
    };
    struct FloatFill {
        bool on = false;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        float value = 0.0f;
        std::uint64_t generation = 0;
    };
    FloatFill floatFill_;
    bool FloatFilled(const Resource& texture) const;
    Resource* SameMemoryLocked(std::uint64_t address, const D3D12_RESOURCE_DESC& applied);
    std::set<std::pair<std::pair<int, int>, bool>> sameMemoryPairs_;
    std::uint64_t sameMemoryShared_ = 0;
    std::uint64_t sameMemoryApart_ = 0;
    static constexpr std::uint32_t kLiveAfterChecks = 16;
    std::set<Resource*> live_;
    bool liveCheck_ = false;
    std::uint64_t liveUploads_ = 0;
    std::chrono::steady_clock::time_point liveNext_{};
    std::uint64_t liveGroupUs_ = 0;
    std::uint64_t liveGroups_ = 0;
    std::uint64_t liveGroupBytes_ = 0;
    struct UploadParts {
        std::uint64_t readUs = 0;
        std::uint64_t readBytes = 0;
        std::uint64_t detileUs = 0;
        std::uint64_t stagingUs = 0;
        std::uint64_t stagingWaits = 0;
        std::uint64_t ownStaging = 0;
    };
    UploadParts uploadParts_;
    UploadParts liveParts_;
    std::uint64_t livePartGroups_ = 0;
    std::uint64_t livePartUs_ = 0;
    std::uint64_t livePartBytes_ = 0;
    std::uint64_t livePartTextures_ = 0;
    std::vector<std::uint8_t> scratch_;
    void RefreshLiveLocked();
    std::uint64_t settleWaits_ = 0;
    std::uint64_t frame_ = 0;
    Resource* resumeAfter_ = nullptr;
    std::map<Resource*, std::pair<std::uint64_t, std::vector<std::uint8_t>>> keys_;
    std::vector<std::pair<Resource*, std::uint64_t>> graveyard_;
    std::uint64_t evicted_ = 0;
    std::map<Resource*, std::uint32_t> skippedMips_;
    std::vector<std::function<bool(CommandRecorder&, std::string&)>> batch_;
    std::vector<std::function<void()>> cleanup_;
    std::uint64_t batchBytes_ = 0;
    struct InFlight {
        VulkanDevice::PendingSubmit submit;
        std::vector<std::function<void()>> cleanup;
        std::uint64_t bytes = 0;
    };
    std::deque<InFlight> inFlight_;
    std::uint64_t inFlightBytes_ = 0;
    bool RetireOldest(bool wait);
    void ReapLocked(bool wait);
    std::optional<std::uint64_t> StagingLocked(std::uint64_t bytes);
    void FlushLocked();
    void EvictOverlapping(Resource* kept, std::uint64_t address, std::uint64_t bytes);
    std::multimap<std::uint64_t, Resource*> byAddress_;
    std::uint64_t largestBytes_ = 0;
    bool Visit(Resource* texture, struct Tracked& tracked, bool& uploaded);
    std::uint64_t refreshMicroseconds_ = 0;
    std::multimap<std::uint64_t, Resource*> recheckDue_;
    struct RecheckState {
        std::uint32_t step = 0;
        bool queued = false;
    };
    std::map<Resource*, RecheckState> recheckStep_;
    bool refreshing_ = false;
    bool deferred_ = false;
    std::uint64_t frameUploadBytes_ = 0;
    std::uint64_t deferredUploads_ = 0;
    std::uint64_t visitCount_ = 0;
    std::uint64_t visitNotMapped_ = 0;
    std::uint64_t visitAccessUs_ = 0;
    std::uint64_t visitHashUs_ = 0;
    std::uint64_t visitHashIndirectUs_ = 0;
    std::uint64_t visitUploadUs_ = 0;
    void ScheduleRecheck(Resource* texture, bool restart, bool nextFrame = false);
    void RunRechecks();
    std::map<Resource*, Tracked> tracked_;
    std::map<Resource*, std::shared_ptr<const gfx10::ConsoleTexture>> layouts_;
    std::set<std::string> linearReasons_;
    static inline std::atomic<TitleTextures*> active_{nullptr};
};

}
