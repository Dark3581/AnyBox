#pragma once

#include "Gfx10Tiling.hpp"
#include "CommandRecorder.hpp"
#include "Resource.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <optional>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <string>
#include <vector>

namespace abx::d3d12x {

class TitleTextures {
public:
    TitleTextures(const ResourceContext& context, IUnknown* parent) : context_(context), parent_(parent) { active_.store(this); }
    static TitleTextures* Active() { return active_.load(); }
    ~TitleTextures();
    TitleTextures(const TitleTextures&) = delete;
    TitleTextures& operator=(const TitleTextures&) = delete;

    Resource* At(std::uint64_t address, const D3D12_RESOURCE_DESC& desc, std::string& error);
    void Written(std::uint64_t start, std::uint64_t bytes);
    void Refresh();
    void Flush();
    void Report();

private:
    enum class Filled { Yes, NotMapped, Failed };
    Filled Upload(Resource& texture, std::uint64_t address, std::uint32_t mips, std::string& error, bool filledBefore = false);
    bool MakeReadable(Resource& texture, std::string& error);

    ResourceContext context_;
    IUnknown* parent_;
    std::mutex lock_;
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
    };
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
    std::map<Resource*, Tracked> tracked_;
    std::map<Resource*, std::shared_ptr<const gfx10::ConsoleTexture>> layouts_;
    std::set<std::string> linearReasons_;
    static inline std::atomic<TitleTextures*> active_{nullptr};
};

}
