#include "TitleTextures.hpp"

#include "AbxApi.hpp"
#include "LiveStats.hpp"
#include "BindlessSet.hpp"
#include "BufferArena.hpp"
#include "CopyLayout.hpp"
#include "TextureDesc.hpp"
#include "TitleAddressSpace.hpp"
#include "TitleBufferWindow.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <set>

namespace abx::d3d12x {

namespace {

constexpr D3D12_RESOURCE_STATES kShaderRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

bool Readable(std::uint64_t address, std::uint64_t bytes) {
    std::uint64_t at = address;
    while (at < address + bytes) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            return false;
        }
        at = reinterpret_cast<std::uint64_t>(region.BaseAddress) + region.RegionSize;
    }
    return true;
}

}

namespace {

const TitleAddressSpace* g_pageSource = nullptr;

bool Accessible(std::uint64_t address, std::uint64_t bytes) {
    if (Readable(address, bytes)) return true;
    if (g_pageSource == nullptr || bytes == 0) return false;
    constexpr std::uint64_t kPage = 0x10000;
    for (std::uint64_t at = address & ~(kPage - 1); at < address + bytes; at += kPage) {
        if (g_pageSource->HostAt(at) == nullptr && !Readable(at, kPage)) return false;
    }
    return true;
}

bool Gather(std::uint64_t address, std::uint64_t bytes, std::vector<std::uint8_t>& out) {
    constexpr std::uint64_t kPage = 0x10000;
    out.resize(static_cast<std::size_t>(bytes));
    std::uint64_t done = 0;
    while (done < bytes) {
        const std::uint64_t at = address + done;
        const std::uint64_t piece = (std::min)(kPage - (at & (kPage - 1)), bytes - done);
        const std::uint8_t* host = g_pageSource != nullptr ? g_pageSource->HostAt(at) : nullptr;
        if (host == nullptr) {
            if (!Readable(at, piece)) return false;
            host = reinterpret_cast<const std::uint8_t*>(at);
        }
        std::memcpy(out.data() + done, host, static_cast<std::size_t>(piece));
        done += piece;
    }
    return true;
}

std::uint64_t WholeHash(std::uint64_t address, std::uint64_t bytes) {
    const auto* words = reinterpret_cast<const std::uint64_t*>(address);
    const std::uint64_t count = bytes / sizeof(std::uint64_t);
    std::uint64_t a = 0x9e3779b97f4a7c15ull;
    std::uint64_t b = 0xc2b2ae3d27d4eb4full;
    std::uint64_t index = 0;
    for (; index + 1 < count; index += 2) {
        a = (a ^ words[index]) * 0x100000001b3ull;
        b = (b ^ words[index + 1]) * 0x100000001b3ull;
    }
    if (index < count) a = (a ^ words[index]) * 0x100000001b3ull;
    return a ^ (b << 1) ^ (b >> 63);
}

std::uint64_t SampleHash(std::uint64_t address, std::uint64_t bytes);

std::uint64_t ContentHash(std::uint64_t address, std::uint64_t bytes, bool whole, bool direct = true) {
    if (!direct) {
        constexpr std::uint64_t kSample = 16;
        constexpr std::uint64_t kSamples = 256;
        const std::uint64_t step = (std::max)(std::uint64_t{4096}, (bytes / kSamples + 15) / 16 * 16);
        std::uint64_t hash = 0xcbf29ce484222325ull;
        for (std::uint64_t at = 0; at + kSample <= bytes; at += step) {
            const std::uint8_t* host = g_pageSource != nullptr ? g_pageSource->HostAt(address + at) : nullptr;
            if (host == nullptr) continue;
            for (std::uint64_t index = 0; index < kSample; ++index) {
                hash ^= host[index];
                hash *= 0x100000001b3ull;
            }
        }
        return hash;
    }
    return whole ? WholeHash(address, bytes) : SampleHash(address, bytes);
}

std::uint64_t SampleHash(std::uint64_t address, std::uint64_t bytes) {
    constexpr std::uint64_t kSample = 16;
    constexpr std::uint64_t kSamples = 256;
    const std::uint64_t kStep = (std::max)(std::uint64_t{4096}, (bytes / kSamples + 15) / 16 * 16);
    std::uint64_t hash = 0xcbf29ce484222325ull;
    const auto* base = reinterpret_cast<const std::uint8_t*>(address);
    for (std::uint64_t at = 0; at + kSample <= bytes; at += kStep) {
        for (std::uint64_t index = 0; index < kSample; ++index) {
            hash ^= base[at + index];
            hash *= 0x100000001b3ull;
        }
    }
    return hash;
}

}

TitleTextures::~TitleTextures() {
    FlushLocked();
    ReapLocked(true);
    for (auto& [texture, stamp] : graveyard_) texture->Release();
    for (auto& [key, texture] : textures_) texture->Release();
}

Resource* TitleTextures::At(std::uint64_t address, const D3D12_RESOURCE_DESC& desc, std::string& error) {
    D3D12_RESOURCE_DESC applied = desc;
    applied.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    applied.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<UINT>(applied.Flags) & 0xFFFFu);
    applied.Alignment = 0;
    std::vector<std::uint8_t> key(sizeof(applied));
    std::memcpy(key.data(), &applied, sizeof(applied));
    const std::lock_guard guard(lock_);
    const auto known = textures_.find({address, key});
    if (known != textures_.end()) {
        const auto waiting = pending_.find(known->second);
        if (waiting != pending_.end()) {
            std::string retry;
            const Filled filled = Upload(*known->second, waiting->second.first, waiting->second.second, retry);
            if (filled == Filled::Yes) {
                pending_.erase(waiting);
                if (tracked_.count(known->second) != 0) ScheduleRecheck(known->second, true);
            }
        }
        const auto used = tracked_.find(known->second);
        if (used != tracked_.end()) used->second.lastUsed = frame_;
        return known->second;
    }
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    const std::uint32_t fullMips = applied.MipLevels != 0 ? applied.MipLevels : FullMipChain(applied.Width, applied.Height, applied.DepthOrArraySize);
    D3D12_RESOURCE_DESC made = applied;
    std::uint32_t skip = 0;
    {
        static const std::uint64_t reduceAbove = [] {
            const char* value = AbxSetting("d3d12_x.texture_reduce_above_mib");
            const unsigned long long mebibytes = value != nullptr ? std::strtoull(value, nullptr, 10) : 4608ull;
            return static_cast<std::uint64_t>(mebibytes) << 20;
        }();
        constexpr UINT kWritten = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        std::string ignored;
        const bool large = applied.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && fullMips > 2 && (applied.Width >= 512 || applied.Height >= 512) &&
                           (static_cast<UINT>(applied.Flags) & kWritten) == 0;
        if (large && reduceAbove != 0 && Stats().textureBytes.load() > reduceAbove &&
            gfx10::DescribeConsoleTexture(desc, gfx10::ConsolePipes(AbxSetting("runtime.console_model")), ignored)) {
            skip = Stats().textureBytes.load() > reduceAbove + (1ull << 30) && (applied.Width >= 1024 || applied.Height >= 1024) ? 2u : 1u;
            made.Width = (std::max)(applied.Width >> skip, UINT64{1});
            made.Height = (std::max)(applied.Height >> skip, 1u);
            made.MipLevels = static_cast<UINT16>(fullMips - skip);
            static std::atomic<std::uint64_t> reduced{0};
            const std::uint64_t count = ++reduced;
            if (count == 1 || (count & (count - 1)) == 0) {
                AbxLog("d3d12_x", "texture memory is past %llu MiB: large title textures are kept without their top mip (a quarter of the memory, slightly blurrier) - %llu so far (d3d12_x.texture_reduce_above_mib, 0 turns it off)",
                       static_cast<unsigned long long>(reduceAbove >> 20), static_cast<unsigned long long>(count));
            }
        }
    }
    Resource* texture = nullptr;
    if (FAILED(Resource::CreateCommitted(context_, parent_, heap, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, made, D3D12_RESOURCE_STATE_COPY_DEST, __uuidof(ID3D12Resource),
                                         reinterpret_cast<void**>(&texture), error))) {
        return nullptr;
    }
    if (skip != 0) {
        skippedMips_[texture] = skip;
        texture->SetSkippedMips(skip);
    }
    const std::uint32_t mips = fullMips - skip;
    {
        std::string why;
        std::optional<gfx10::ConsoleTexture> console = gfx10::DescribeConsoleTexture(desc, gfx10::ConsolePipes(AbxSetting("runtime.console_model")), why);
        if (console && console->mips != fullMips) {
            why = "the console layout has " + std::to_string(console->mips) + " mips and the texture " + std::to_string(fullMips);
            console.reset();
        }
        if (console && console->size == 0) {
            why = "the console layout gives the texture no bytes";
            console.reset();
        }
        if (console) {
            layouts_[texture] = std::make_shared<const gfx10::ConsoleTexture>(std::move(*console));
        } else if (linearReasons_.insert(why).second) {
            AbxLog("d3d12_x", "title texture at 0x%llx (%llux%u, format %d, layout 0x%x) is copied as linear rows, not detiled: %s", static_cast<unsigned long long>(address),
                   static_cast<unsigned long long>(desc.Width), desc.Height, static_cast<int>(desc.Format), static_cast<unsigned>(desc.Layout), why.c_str());
        }
        std::uint64_t bytes = 0;
        const auto layout = layouts_.find(texture);
        if (layout != layouts_.end()) {
            bytes = layout->second->size;
        } else {
            std::string ignored;
            const UINT layers = applied.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : applied.DepthOrArraySize;
            const std::optional<Footprints> footprints = DesktopCopyableFootprints(applied, mips, 0, mips * layers, 0, ignored);
            if (footprints) bytes = footprints->totalBytes;
        }
        if (bytes != 0) {
            if (g_pageSource == nullptr) g_pageSource = context_.addresses;
            const bool readable = Accessible(address, bytes);
            constexpr UINT kTargetFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            constexpr std::uint64_t kWholeHashBytes = 1ull << 20;
            const bool whole = (static_cast<UINT>(desc.Flags) & kTargetFlags) != 0 && bytes <= kWholeHashBytes;
            const bool direct = readable && Readable(address, bytes) && (g_pageSource == nullptr || g_pageSource->HostAt(address) == nullptr);
            tracked_[texture] = Tracked{address, bytes, mips, readable ? ContentHash(address, bytes, whole, direct) : 0, readable, whole, direct};
            tracked_[texture].lastUsed = frame_;
            byAddress_.emplace(address, texture);
            largestBytes_ = (std::max)(largestBytes_, bytes);
            keys_[texture] = std::make_pair(address, key);
            EvictOverlapping(texture, address, bytes);
        }
    }
    const Filled filled = Upload(*texture, address, mips, error);
    if (filled == Filled::Yes && tracked_.count(texture) != 0) ScheduleRecheck(texture, true);
    if (filled == Filled::Failed) {
        tracked_.erase(texture);
        layouts_.erase(texture);
        keys_.erase(texture);
        for (auto range = byAddress_.equal_range(address); range.first != range.second; ++range.first) {
            if (range.first->second == texture) {
                byAddress_.erase(range.first);
                break;
            }
        }
        texture->Release();
        return nullptr;
    }
    if (filled == Filled::NotMapped) {
        if (!MakeReadable(*texture, error)) {
            tracked_.erase(texture);
            layouts_.erase(texture);
            keys_.erase(texture);
            for (auto range = byAddress_.equal_range(address); range.first != range.second; ++range.first) {
                if (range.first->second == texture) {
                    byAddress_.erase(range.first);
                    break;
                }
            }
            texture->Release();
            return nullptr;
        }
        pending_[texture] = {address, mips};
        static bool reported = false;
        if (!reported) {
            reported = true;
            AbxLog("d3d12_x", "a texture at title address 0x%llx has memory that is not mapped yet (%s); it is created empty and filled when a later view finds the memory mapped",
                   static_cast<unsigned long long>(address), error.c_str());
        }
        error.clear();
    }
    textures_.emplace(std::make_pair(address, std::move(key)), texture);
    AbxTrace("title texture at 0x%llx: %llux%u, %u mips, format %d uploaded from title memory (later writes there are not seen yet)", static_cast<unsigned long long>(address),
             static_cast<unsigned long long>(applied.Width), applied.Height, mips, applied.Format);
    return texture;
}

bool TitleTextures::MakeReadable(Resource& texture, std::string& error) {
    bool recorded = true;
    const bool submitted = context_.device.Submit(
        [&](VkCommandBuffer commands) {
            CommandRecorder recorder(context_.device, commands);
            recorded = recorder.Transition(texture.Texture(), D3D12_RESOURCE_STATE_COPY_DEST, kShaderRead, error);
        },
        error);
    return submitted && recorded;
}

void TitleTextures::Report() {
    const std::lock_guard guard(lock_);
    std::size_t changed = 0, empty = 0, filled = 0, unreadable = 0;
    for (const auto& [texture, tracked] : tracked_) {
        if (!Readable(tracked.address, tracked.bytes)) {
            ++unreadable;
            continue;
        }
        const auto* base = reinterpret_cast<const std::uint8_t*>(tracked.address);
        std::uint64_t nonzero = 0, samples = 0;
        for (std::uint64_t at = 0; at < tracked.bytes; at += 4096, ++samples) nonzero += base[at] != 0 || base[(std::min)(at + 7, tracked.bytes - 1)] != 0;
        const bool now = ContentHash(tracked.address, tracked.bytes, tracked.whole) != tracked.sample;
        changed += now;
        ++(nonzero == 0 ? empty : filled);
        const D3D12_RESOURCE_DESC& desc = texture->Texture().desc;
        if (desc.Width >= 512 && (now || nonzero == 0)) {
            AbxLog("d3d12_x", "DIAG title texture 0x%llx %llux%u f%d mips %u: memory %s, %llu of %llu samples nonzero, %s since its upload%s", static_cast<unsigned long long>(tracked.address),
                   static_cast<unsigned long long>(desc.Width), desc.Height, static_cast<int>(desc.Format), tracked.mips, nonzero == 0 ? "all zero" : "has data",
                   static_cast<unsigned long long>(nonzero), static_cast<unsigned long long>(samples), now ? "CHANGED" : "unchanged", pending_.count(texture) ? " (pending)" : "");
        }
    }
    AbxLog("d3d12_x", "DIAG title textures: %zu tracked, %zu with data, %zu all zero, %zu not readable, %zu changed since upload", tracked_.size(), filled, empty, unreadable, changed);
}

bool TitleTextures::Visit(Resource* texture, Tracked& tracked, bool& uploaded) {
    uploaded = false;
    if (!tracked.readable) {
        if (!Accessible(tracked.address, tracked.bytes)) return true;
        tracked.readable = true;
        tracked.direct = Readable(tracked.address, tracked.bytes) && (g_pageSource == nullptr || g_pageSource->HostAt(tracked.address) == nullptr);
    }
    const std::uint64_t sample = ContentHash(tracked.address, tracked.bytes, tracked.whole, tracked.direct);
    if (sample == tracked.sample) return true;
    std::string error;
    const bool waiting = pending_.count(texture) != 0;
    const Filled filled = Upload(*texture, tracked.address, tracked.mips, error, !waiting);
    if (tracked.whole) {
        static std::atomic<std::uint64_t> wholeChanges{0};
        const std::uint64_t changes = ++wholeChanges;
        if (changes <= 4 || (changes & (changes - 1)) == 0) {
            const D3D12_RESOURCE_DESC& changed = texture->Texture().desc;
            AbxLog("d3d12_x", "a render-target title texture at 0x%llx (%llux%u, format %d) changed in title memory and was uploaded again%s%s (%llu so far)",
                   static_cast<unsigned long long>(tracked.address), static_cast<unsigned long long>(changed.Width), changed.Height, static_cast<int>(changed.Format),
                   filled == Filled::Yes ? "" : ", but the upload failed: ", filled == Filled::Yes ? "" : error.c_str(), static_cast<unsigned long long>(changes));
        }
    }
    if (filled != Filled::Yes) {
        std::string reason;
        for (const char ch : error) {
            if (ch < '0' || ch > '9') reason.push_back(ch);
        }
        static std::mutex reasonLock;
        static std::set<std::string> reasons;
        const std::lock_guard reasonGuard(reasonLock);
        if (reasons.insert(reason).second) AbxLog("d3d12_x", "a title texture at 0x%llx changed but could not be uploaded again (later cases of this reason are not logged): %s", static_cast<unsigned long long>(tracked.address), error.c_str());
        return true;
    }
    if (waiting) pending_.erase(texture);
    tracked.sample = sample;
    uploaded = true;
    ScheduleRecheck(texture, true);
    return true;
}

void TitleTextures::ScheduleRecheck(Resource* texture, bool restart) {
    static constexpr std::uint64_t kAfterFrames[] = {4, 12, 40, 120, 400};
    constexpr std::uint32_t kSteps = sizeof(kAfterFrames) / sizeof(kAfterFrames[0]);
    std::uint32_t& step = recheckStep_[texture];
    step = restart ? 0 : step + 1;
    if (step >= kSteps) {
        recheckStep_.erase(texture);
        return;
    }
    recheckDue_.emplace(frame_ + kAfterFrames[step], texture);
}

void TitleTextures::RunRechecks() {
    constexpr std::size_t kPerFrame = 512;
    std::size_t done = 0;
    while (!recheckDue_.empty() && recheckDue_.begin()->first <= frame_ && done < kPerFrame) {
        Resource* const texture = recheckDue_.begin()->second;
        recheckDue_.erase(recheckDue_.begin());
        ++done;
        const auto tracked = tracked_.find(texture);
        if (tracked == tracked_.end() || recheckStep_.count(texture) == 0) continue;
        bool uploaded = false;
        Visit(texture, tracked->second, uploaded);
        if (!uploaded) {
            ScheduleRecheck(texture, false);
            continue;
        }
        static std::atomic<std::uint64_t> changed{0};
        const std::uint64_t count = ++changed;
        if (count == 1 || (count & (count - 1)) == 0) {
            AbxLog("d3d12_x", "a title texture checked again a few frames after its upload had changed in title memory and was uploaded again (%llu so far): titles often fill "
                              "a texture's memory after creating it",
                   static_cast<unsigned long long>(count));
        }
    }
}

void TitleTextures::EvictOverlapping(Resource* kept, std::uint64_t address, std::uint64_t bytes) {
    constexpr std::uint64_t kQuietFrames = 8;
    const std::uint64_t from = address > largestBytes_ ? address - largestBytes_ : 0;
    std::vector<Resource*> victims;
    for (auto entry = byAddress_.lower_bound(from); entry != byAddress_.end() && entry->first < address + bytes; ++entry) {
        Resource* const other = entry->second;
        if (other == kept) continue;
        const auto found = tracked_.find(other);
        if (found == tracked_.end()) continue;
        const Tracked& tracked = found->second;
        if (tracked.address + tracked.bytes <= address || tracked.address >= address + bytes) continue;
        if (tracked.lastUsed + kQuietFrames > frame_) continue;
        victims.push_back(other);
    }
    for (Resource* victim : victims) {
        const auto key = keys_.find(victim);
        if (key != keys_.end()) {
            textures_.erase(key->second);
            keys_.erase(key);
        }
        const auto tracked = tracked_.find(victim);
        if (tracked != tracked_.end()) {
            for (auto range = byAddress_.equal_range(tracked->second.address); range.first != range.second; ++range.first) {
                if (range.first->second == victim) {
                    byAddress_.erase(range.first);
                    break;
                }
            }
            tracked_.erase(tracked);
        }
        layouts_.erase(victim);
        skippedMips_.erase(victim);
        recheckStep_.erase(victim);
        pending_.erase(victim);
        if (resumeAfter_ == victim) resumeAfter_ = nullptr;
        graveyard_.emplace_back(victim, frame_);
        if (++evicted_ == 1) {
            AbxLog("d3d12_x", "a title texture whose memory the title placed a new texture over is released (after %llu quiet frames, 4 frames later): on the console a texture is its memory",
                   static_cast<unsigned long long>(kQuietFrames));
        }
    }
}

void TitleTextures::Refresh() {
    constexpr auto kBudget = std::chrono::microseconds(2000);
    const auto started = std::chrono::steady_clock::now();
    const std::lock_guard guard(lock_);
    std::size_t refreshed = 0;
    ++frame_;
    bool uploaded = false;
    FlushLocked();
    for (auto grave = graveyard_.begin(); grave != graveyard_.end();) {
        if (grave->second + 4 > frame_) {
            ++grave;
            continue;
        }
        grave->first->Release();
        grave = graveyard_.erase(grave);
    }
    RunRechecks();
    for (auto& [texture, tracked] : tracked_) {
        if (!tracked.whole || !tracked.readable) continue;
        if (((frame_ + (reinterpret_cast<std::uintptr_t>(texture) >> 6)) & 3) != 0) continue;
        Visit(texture, tracked, uploaded);
        refreshed += uploaded;
    }
    std::size_t visited = 0;
    auto it = tracked_.upper_bound(resumeAfter_);
    for (std::size_t steps = 0; steps < tracked_.size(); ++steps) {
        if (it == tracked_.end()) it = tracked_.begin();
        if (it == tracked_.end()) break;
        if (!it->second.whole || !it->second.readable) {
            Visit(it->first, it->second, uploaded);
            refreshed += uploaded;
            ++visited;
        }
        resumeAfter_ = it->first;
        ++it;
        if ((visited & 7) == 0 && std::chrono::steady_clock::now() - started > kBudget) break;
    }
    static std::atomic<std::uint64_t> total{0};
    if (refreshed != 0 && total.fetch_add(refreshed) == 0) {
        AbxLog("d3d12_x", "title textures are uploaded again when their memory changes (glyph-cache render targets hashed whole every 4 frames, the rest from 256 samples each, round-robin within 2 ms a frame)");
    }
    FlushLocked();
    refreshMicroseconds_ += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    if (frame_ % 300 == 0) {
        AbxLog("d3d12_x", "title texture refresh: %llu textures tracked, %llu uploaded again so far, %llu released for reused memory, %.2f ms a frame", static_cast<unsigned long long>(tracked_.size()),
               static_cast<unsigned long long>(total.load()), static_cast<unsigned long long>(evicted_), static_cast<double>(refreshMicroseconds_) / 300000.0);
        refreshMicroseconds_ = 0;
    }
}

void TitleTextures::Written(std::uint64_t start, std::uint64_t bytes) {
    const std::lock_guard guard(lock_);
    const std::uint64_t from = start > largestBytes_ ? start - largestBytes_ : 0;
    for (auto entry = byAddress_.lower_bound(from); entry != byAddress_.end() && entry->first < start + bytes; ++entry) {
        const auto found = tracked_.find(entry->second);
        if (found == tracked_.end()) continue;
        Resource* const texture = found->first;
        Tracked& tracked = found->second;
        if (tracked.address >= start + bytes || tracked.address + tracked.bytes <= start) continue;
        std::string error;
        const bool waiting = pending_.count(texture) != 0;
        const Filled filled = Upload(*texture, tracked.address, tracked.mips, error, !waiting);
        if (filled == Filled::Yes && waiting) pending_.erase(texture);
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            AbxLog("d3d12_x", "title textures are uploaded again when DirectStorage writes their memory (first: 0x%llx, %llu bytes)%s%s",
                   static_cast<unsigned long long>(tracked.address), static_cast<unsigned long long>(tracked.bytes), filled == Filled::Yes ? "" : "; that upload failed: ",
                   filled == Filled::Yes ? "" : error.c_str());
        }
    }
}

TitleTextures::Filled TitleTextures::Upload(Resource& texture, std::uint64_t address, std::uint32_t mips, std::string& error, bool filledBefore) {
    const D3D12_RESOURCE_DESC& desc = texture.Texture().desc;
    const UINT layers = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
    const UINT count = mips * layers;
    const std::optional<Footprints> footprints = DesktopCopyableFootprints(desc, mips, 0, count, 0, error);
    if (!footprints) return Filled::Failed;
    const auto described = layouts_.find(&texture);
    const auto skipped = skippedMips_.find(&texture);
    const std::uint32_t skip = skipped != skippedMips_.end() ? skipped->second : 0;
    const gfx10::ConsoleTexture* console = described != layouts_.end() ? described->second.get() : nullptr;
    const std::uint64_t titleBytes = console != nullptr ? console->size : footprints->totalBytes;
    if (g_pageSource == nullptr) g_pageSource = context_.addresses;
    if (!Accessible(address, titleBytes)) {
        error = "the texture's title memory is not readable for " + std::to_string(titleBytes) + " bytes";
        return Filled::NotMapped;
    }
    std::vector<std::uint8_t> gathered;
    const bool direct = Readable(address, titleBytes);
    if (!direct && !Gather(address, titleBytes, gathered)) {
        error = "the texture's GPU-only title memory could not be read";
        return Filled::NotMapped;
    }
    const bool snapshot = direct && (console != nullptr || !context_.addresses || !context_.addresses->Locate(address, footprints->totalBytes));
    if (snapshot) {
        gathered.resize(static_cast<std::size_t>(titleBytes));
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), gathered.data(), gathered.size(), &read) || read != gathered.size()) {
            static std::atomic<int> reported{0};
            if (reported.fetch_add(1) < 4) {
                AbxLog("d3d12_x", "a title texture's memory at 0x%llx went away while it was being uploaded (the title released it); the upload is dropped",
                       static_cast<unsigned long long>(address));
            }
            error = "the texture's title memory went away during the upload";
            return Filled::NotMapped;
        }
    }
    const std::uint64_t source = (direct && !snapshot) ? address : reinterpret_cast<std::uint64_t>(gathered.data());
    const bool alreadyReadable = filledBefore || pending_.count(&texture) != 0;
    std::optional<std::uint64_t> offset;
    BufferRef staging;
    DeviceBuffer hostStaging;
    std::optional<TitleAddressSpace::Location> imported;
    if (console != nullptr) {
        std::uint8_t* linear = nullptr;
        offset = StagingLocked(footprints->totalBytes);
        if (offset) {
            linear = context_.window.Base() + *offset;
            staging = {context_.window.Buffer(), *offset, footprints->totalBytes};
        } else {
            if (!CreateHostBuffer(context_.device, footprints->totalBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hostStaging, error)) return Filled::Failed;
            linear = static_cast<std::uint8_t*>(hostStaging.mapped);
            staging = {hostStaging.buffer, 0, footprints->totalBytes};
        }
        const auto* texels = reinterpret_cast<const std::uint8_t*>(source) + console->texelOffset;
        const std::size_t texelBytes = static_cast<std::size_t>((std::min)(console->texelBytes, console->size - console->texelOffset));
        bool detiled = true;
        for (UINT subresource = 0; subresource < count && detiled; ++subresource) {
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = footprints->layouts[subresource];
            detiled = gfx10::DetileSubresource(*console, texels, texelBytes, subresource % mips + skip, subresource / mips, linear + footprint.Offset, footprint.Footprint.RowPitch,
                                               footprints->rows[subresource], static_cast<std::size_t>(footprints->rowBytes[subresource]), error);
        }
        if (!detiled) {
            if (offset) context_.arena.Free(*offset);
            if (hostStaging.buffer != VK_NULL_HANDLE) DestroyBuffer(context_.device, hostStaging);
            return Filled::Failed;
        }
    } else if (context_.addresses != nullptr) {
        imported = context_.addresses->Locate(address, footprints->totalBytes);
    }
    if (console == nullptr && imported) {
        staging = {imported->buffer, imported->offset, footprints->totalBytes};
    } else if (console == nullptr) {
        offset = StagingLocked(footprints->totalBytes);
        if (!offset) {
            error = "the buffer window has no room to stage a texture of " + std::to_string(footprints->totalBytes) + " bytes, and the texture is not in imported title memory";
            return Filled::Failed;
        }
        std::memcpy(context_.window.Base() + *offset, reinterpret_cast<const void*>(source), static_cast<std::size_t>(footprints->totalBytes));
        staging = {context_.window.Buffer(), *offset, footprints->totalBytes};
    }
    Resource* const target = &texture;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(footprints->layouts.begin(), footprints->layouts.begin() + count);
    batch_.push_back([target, layouts = std::move(layouts), staging, alreadyReadable](CommandRecorder& recorder, std::string& failure) {
        bool recorded = true;
        if (alreadyReadable) recorded = recorder.Transition(target->Texture(), kShaderRead, D3D12_RESOURCE_STATE_COPY_DEST, failure);
        for (UINT subresource = 0; subresource < layouts.size() && recorded; ++subresource) {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = layouts[subresource];
            recorded = recorder.CopyBufferToTexture(target->Texture(), D3D12_RESOURCE_STATE_COPY_DEST, subresource, 0, 0, 0, staging, footprint, nullptr, failure);
        }
        if (recorded) recorded = recorder.Transition(target->Texture(), D3D12_RESOURCE_STATE_COPY_DEST, kShaderRead, failure);
        return recorded;
    });
    if (offset || hostStaging.buffer != VK_NULL_HANDLE) {
        const std::optional<std::uint64_t> arenaOffset = offset;
        DeviceBuffer host = hostStaging;
        cleanup_.push_back([this, arenaOffset, host]() mutable {
            if (arenaOffset) context_.arena.Free(*arenaOffset);
            if (host.buffer != VK_NULL_HANDLE) DestroyBuffer(context_.device, host);
        });
    }
    target->AddRef();
    cleanup_.push_back([target] { target->Release(); });
    batchBytes_ += footprints->totalBytes;
    if (batchBytes_ > (256ull << 20)) FlushLocked();
    return Filled::Yes;
}

std::optional<std::uint64_t> TitleTextures::StagingLocked(std::uint64_t bytes) {
    std::optional<std::uint64_t> offset = context_.arena.Allocate(bytes, kBufferPlacementAlignment);
    if (!offset && !inFlight_.empty()) {
        ReapLocked(true);
        offset = context_.arena.Allocate(bytes, kBufferPlacementAlignment);
    }
    return offset;
}

bool TitleTextures::RetireOldest(bool wait) {
    if (inFlight_.empty()) return false;
    InFlight& oldest = inFlight_.front();
    const VkResult state = context_.device.Finish(oldest.submit, wait);
    if (state == VK_NOT_READY) return false;
    if (state != VK_SUCCESS) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) AbxLog("d3d12_x", "a batch of title texture uploads did not finish on the GPU (VkResult %d)", static_cast<int>(state));
    }
    for (auto& clean : oldest.cleanup) clean();
    inFlightBytes_ -= oldest.bytes;
    inFlight_.pop_front();
    return true;
}

void TitleTextures::ReapLocked(bool wait) {
    while (RetireOldest(wait)) {
    }
}

void TitleTextures::FlushLocked() {
    ReapLocked(false);
    if (batch_.empty()) return;
    std::string error;
    bool recorded = true;
    std::optional<VulkanDevice::PendingSubmit> submitted = context_.device.SubmitWithoutWaiting(
        [&](VkCommandBuffer commands) {
            CommandRecorder recorder(context_.device, commands);
            for (const auto& record : batch_) {
                if (!record(recorder, error)) recorded = false;
            }
        },
        error);
    if (submitted) {
        inFlightBytes_ += batchBytes_;
        inFlight_.push_back(InFlight{*submitted, std::move(cleanup_), batchBytes_});
    } else {
        for (auto& clean : cleanup_) clean();
    }
    static std::atomic<bool> reported{false};
    if ((!submitted || !recorded) && !reported.exchange(true)) {
        AbxLog("d3d12_x", "a batch of %zu title texture uploads failed: %s", batch_.size(), error.c_str());
    }
    static std::atomic<bool> announced{false};
    if (!announced.exchange(true)) {
        AbxLog("d3d12_x", "title texture uploads are batched into one GPU submission before the title's next ExecuteCommandLists or present, without waiting for the GPU; staging memory "
                          "is freed once the batch's fence signals (first batch: %zu textures)", batch_.size());
    }
    batch_.clear();
    cleanup_.clear();
    batchBytes_ = 0;
    constexpr std::size_t kMaxInFlight = 8;
    constexpr std::uint64_t kMaxInFlightBytes = 512ull << 20;
    while (inFlight_.size() > kMaxInFlight || inFlightBytes_ > kMaxInFlightBytes) {
        if (!RetireOldest(true)) break;
    }
}

void TitleTextures::Flush() {
    const std::lock_guard guard(lock_);
    FlushLocked();
}

}
