#include "TitleTextures.hpp"

#include "FrameTiming.hpp"

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
#include <cstdio>
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

constexpr std::uint64_t kRefreshUploadBytes = 8ull << 20;

bool BlockCompressed(DXGI_FORMAT format) {
    const int value = static_cast<int>(format);
    return (value >= 70 && value <= 84) || (value >= 94 && value <= 99);
}

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
        constexpr std::uint64_t kPage = 0x10000;
        const std::uint64_t step = (std::max)(kSample, (bytes / kSamples + 15) / 16 * 16);
        std::uint64_t hash = 0xcbf29ce484222325ull;
        std::uint64_t page = ~std::uint64_t{0};
        const std::uint8_t* pageHost = nullptr;
        for (std::uint64_t at = 0; at + kSample <= bytes; at += step) {
            const std::uint64_t within = (address + at) & (kPage - 1);
            if (within + kSample > kPage) continue;
            if (address + at - within != page) {
                page = address + at - within;
                pageHost = g_pageSource != nullptr ? g_pageSource->HostAt(page) : nullptr;
            }
            if (pageHost == nullptr) continue;
            const std::uint8_t* host = pageHost + within;
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
    const std::uint64_t kStep = (std::max)(kSample, (bytes / kSamples + 15) / 16 * 16);
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
    if (Resource* const same = SameMemoryLocked(address, applied)) return same;
    const TimedScope timed(TimedPart::TextureMake);
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    const std::uint32_t fullMips = applied.MipLevels != 0 ? applied.MipLevels : FullMipChain(applied.Width, applied.Height, applied.DepthOrArraySize);
    D3D12_RESOURCE_DESC made = applied;
    std::uint32_t skip = 0;
    bool reducible = false;
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
        reducible = large && gfx10::DescribeConsoleTexture(desc, gfx10::ConsolePipes(AbxSetting("runtime.console_model")), ignored).has_value();
        if (reducible && reduceAbove != 0 && Stats().textureBytes.load() > reduceAbove) {
            skip = Stats().textureBytes.load() > reduceAbove + (1ull << 30) && (applied.Width >= 1024 || applied.Height >= 1024) ? 2u : 1u;
            static const std::uint64_t morePercent = [] {
                const char* value = AbxSetting("d3d12_x.texture_reduce_more_at_percent");
                return value != nullptr ? static_cast<std::uint64_t>(std::strtoull(value, nullptr, 10)) : std::uint64_t{0};
            }();
            const std::uint64_t budget = Stats().videoBudget.load(std::memory_order_relaxed);
            if (morePercent != 0 && budget != 0 && Stats().videoUsage.load(std::memory_order_relaxed) >= budget / 100 * morePercent && fullMips - (skip + 1) > 3) {
                ++skip;
                static std::atomic<std::uint64_t> pressed{0};
                const std::uint64_t pressedCount = ++pressed;
                if (pressedCount == 1 || (pressedCount & (pressedCount - 1)) == 0) {
                    AbxLog("d3d12_x", "video memory in use is at %llu%% of the system's budget or more: large title textures made now lose one more top mip, %u in all for this one "
                                      "(%llux%u) - %llu so far (d3d12_x.texture_reduce_more_at_percent, 0 turns it off)",
                           static_cast<unsigned long long>(morePercent), skip, static_cast<unsigned long long>(applied.Width), applied.Height,
                           static_cast<unsigned long long>(pressedCount));
                }
            }
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
    HRESULT created = Resource::CreateCommitted(context_, parent_, heap, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, made, D3D12_RESOURCE_STATE_COPY_DEST, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(&texture), error);
    while (FAILED(created) && reducible && error.find("no device memory") != std::string::npos && skip < 3 && fullMips - skip > 3) {
        ++skip;
        made.Width = (std::max)(applied.Width >> skip, UINT64{1});
        made.Height = (std::max)(applied.Height >> skip, 1u);
        made.MipLevels = static_cast<UINT16>(fullMips - skip);
        static std::atomic<std::uint64_t> retried{0};
        const std::uint64_t count = ++retried;
        if (count == 1 || (count & (count - 1)) == 0) {
            AbxLog("d3d12_x", "video memory is exhausted: a title texture at 0x%llx (%llux%u) is made again without its top %u mip(s) instead of failing (%llu so far)",
                   static_cast<unsigned long long>(address), static_cast<unsigned long long>(applied.Width), applied.Height, skip, static_cast<unsigned long long>(count));
        }
        error.clear();
        texture = nullptr;
        created = Resource::CreateCommitted(context_, parent_, heap, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, made, D3D12_RESOURCE_STATE_COPY_DEST, __uuidof(ID3D12Resource),
                                            reinterpret_cast<void**>(&texture), error);
    }
    if (FAILED(created)) return nullptr;
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
            texture->RememberTitleAddress(address);
            largestBytes_ = (std::max)(largestBytes_, bytes);
            keys_[texture] = std::make_pair(address, key);
            EvictOverlapping(texture, address, bytes);
        }
    }
    const Filled filled = Upload(*texture, address, mips, error);
    if (filled == Filled::Yes && tracked_.count(texture) != 0) {
        tracked_[texture].uploads = 1;
        tracked_[texture].lastUpload = frame_;
        ScheduleRecheck(texture, true);
    }
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

Resource* TitleTextures::SameMemoryLocked(std::uint64_t address, const D3D12_RESOURCE_DESC& applied) {
    constexpr UINT kWritten = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const UINT wanted = static_cast<UINT>(applied.Flags);
    if ((wanted & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 || applied.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return nullptr;
    const std::optional<CopyBlock> block = CopyBlockOf(applied.Format);
    if (!block || block->width != 1 || block->height != 1 || block->bytes == 0) return nullptr;
    const std::uint32_t mips = applied.MipLevels != 0 ? applied.MipLevels : FullMipChain(applied.Width, applied.Height, applied.DepthOrArraySize);
    for (auto range = byAddress_.equal_range(address); range.first != range.second; ++range.first) {
        Resource* const other = range.first->second;
        if (skippedMips_.count(other) != 0) continue;
        const D3D12_RESOURCE_DESC& own = other->Texture().desc;
        const UINT ownFlags = static_cast<UINT>(own.Flags);
        if (((wanted | ownFlags) & kWritten) == 0 || (ownFlags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) continue;
        if (own.Dimension != applied.Dimension || own.Width != applied.Width || own.Height != applied.Height || own.DepthOrArraySize != applied.DepthOrArraySize ||
            own.SampleDesc.Count != applied.SampleDesc.Count || other->Texture().layout.info.mipLevels != mips || !other->Texture().layout.format.mutableFormat) {
            continue;
        }
        const std::optional<CopyBlock> ownBlock = CopyBlockOf(own.Format);
        if (!ownBlock || ownBlock->width != 1 || ownBlock->height != 1 || ownBlock->bytes != block->bytes) continue;
        const VkImageUsageFlags usage = other->Texture().layout.info.usage;
        const bool enough = ((wanted & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0 || (usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0) &&
                            ((wanted & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 || (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0);
        const std::uint64_t count = enough ? ++sameMemoryShared_ : ++sameMemoryApart_;
        const bool fresh = sameMemoryPairs_.insert({{static_cast<int>(applied.Format), static_cast<int>(own.Format)}, enough}).second;
        if (fresh || (count & (count - 1)) == 0) {
            AbxLog("d3d12_x",
                   "a texture at title address 0x%llx (%llux%u, %u mips) that the GPU writes is described again with format %d flags 0x%x over the live format %d flags 0x%x: %s (%llu so far)",
                   static_cast<unsigned long long>(address), static_cast<unsigned long long>(applied.Width), applied.Height, mips, static_cast<int>(applied.Format), wanted,
                   static_cast<int>(own.Format), ownFlags,
                   enough ? "both are one image, as they are one memory on the console, and each view reads it in its own format"
                          : "the live image lacks the Vulkan usage the new description needs, so the new one gets its own image and does not see what is written to the other",
                   static_cast<unsigned long long>(count));
        }
        if (!enough) continue;
        const auto used = tracked_.find(other);
        if (used != tracked_.end()) used->second.lastUsed = frame_;
        return other;
    }
    return nullptr;
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

std::string TitleTextures::Describe(Resource* texture) {
    const std::lock_guard guard(lock_);
    const auto found = tracked_.find(texture);
    if (found == tracked_.end()) return "not kept in step with title memory";
    const Tracked& tracked = found->second;
    char text[360];
    std::snprintf(text, sizeof(text),
                  "kept in step with title memory at 0x%llx (%llu bytes, %s, hashed %s%s), uploaded %llu time(s), last at refresh frame %llu of %llu, changed at %u check(s) in a "
                  "row%s%s%s",
                  static_cast<unsigned long long>(tracked.address), static_cast<unsigned long long>(tracked.bytes), tracked.readable ? "readable" : "not readable",
                  tracked.whole ? "whole" : "by samples", tracked.direct ? "" : " through pages", static_cast<unsigned long long>(tracked.uploads),
                  static_cast<unsigned long long>(tracked.lastUpload), static_cast<unsigned long long>(frame_), tracked.changedChecks,
                  live_.count(texture) != 0 ? ", uploaded as video-like" : "", tracked.settling ? ", waiting for its memory to settle" : "",
                  pending_.count(texture) != 0 ? ", not filled yet" : "");
    return text;
}

bool TitleTextures::FloatFilled(const Resource& texture) const {
    if (!floatFill_.on) return false;
    const D3D12_RESOURCE_DESC& desc = texture.Texture().desc;
    return desc.Format == DXGI_FORMAT_R32_FLOAT && desc.Width == floatFill_.width && desc.Height == floatFill_.height;
}

void TitleTextures::FillFloats(std::uint32_t width, std::uint32_t height, float value, bool on) {
    const std::lock_guard guard(lock_);
    floatFill_.on = on;
    floatFill_.width = width;
    floatFill_.height = height;
    floatFill_.value = value;
    ++floatFill_.generation;
    std::size_t matching = 0;
    for (auto& [texture, tracked] : tracked_) {
        if (tracked.fillGeneration == 0 && !FloatFilled(*texture)) continue;
        ++matching;
        RecheckState& state = recheckStep_[texture];
        state.step = 0;
        state.queued = true;
        recheckDue_.emplace(frame_ + 1, texture);
    }
    if (on) {
        AbxLog("d3d12_x", "DIAG title texture fill: R32_FLOAT title textures of %ux%u are uploaded with every texel at %g instead of their title memory until the fill is turned off "
                          "(%zu tracked now, uploaded again at their next check)",
               width, height, static_cast<double>(value), matching);
    } else {
        AbxLog("d3d12_x", "DIAG title texture fill: off, %zu filled texture(s) are uploaded from their title memory again at their next check", matching);
    }
}

bool TitleTextures::Visit(Resource* texture, Tracked& tracked, bool& uploaded) {
    uploaded = false;
    const auto visitStart = std::chrono::steady_clock::now();
    const auto spent = [](std::uint64_t& counter, std::chrono::steady_clock::time_point since) {
        counter += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since).count());
    };
    ++visitCount_;
    if (!tracked.readable) {
        const bool accessible = Accessible(tracked.address, tracked.bytes);
        spent(visitAccessUs_, visitStart);
        if (!accessible) {
            ++visitNotMapped_;
            return true;
        }
        tracked.readable = true;
        tracked.direct = Readable(tracked.address, tracked.bytes) && (g_pageSource == nullptr || g_pageSource->HostAt(tracked.address) == nullptr);
    }
    const auto hashStart = std::chrono::steady_clock::now();
    const std::uint64_t sample = ContentHash(tracked.address, tracked.bytes, tracked.whole, tracked.direct);
    spent(tracked.direct ? visitHashUs_ : visitHashIndirectUs_, hashStart);
    const std::uint64_t wantedFill = FloatFilled(*texture) ? floatFill_.generation : 0;
    const bool refill = tracked.fillGeneration != wantedFill;
    if (sample == tracked.sample && !refill) {
        tracked.settling = false;
        if (!liveCheck_ && tracked.changedChecks > 0) --tracked.changedChecks;
        return true;
    }
    if (BlockCompressed(texture->Texture().desc.Format)) tracked.changedChecks = 0;
    else if (liveCheck_) tracked.changedChecks = kLiveAfterChecks;
    else if (tracked.changedChecks < kLiveAfterChecks) ++tracked.changedChecks;
    if (tracked.changedChecks >= kLiveAfterChecks && !tracked.whole) live_.insert(texture);
    if (!refill && !liveCheck_ && refreshing_ && !tracked.whole && tracked.changedChecks >= kLiveAfterChecks) return true;
    if (!refill && refreshing_ && !tracked.whole && tracked.changedChecks < kLiveAfterChecks && (!tracked.settling || sample != tracked.candidate)) {
        tracked.settling = true;
        tracked.candidate = sample;
        ++settleWaits_;
        ScheduleRecheck(texture, true);
        return true;
    }
    struct UploadTimer {
        std::uint64_t& counter;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~UploadTimer() { counter += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()); }
    } uploadTimer{visitUploadUs_};
    if (refreshing_ && frameUploadBytes_ >= kRefreshUploadBytes) {
        deferred_ = true;
        ++deferredUploads_;
        return true;
    }
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
    tracked.fillGeneration = wantedFill;
    tracked.settling = false;
    ++tracked.uploads;
    tracked.lastUpload = frame_;
    uploaded = true;
    ScheduleRecheck(texture, true, tracked.changedChecks >= kLiveAfterChecks);
    return true;
}

void TitleTextures::ScheduleRecheck(Resource* texture, bool restart, bool nextFrame) {
    static constexpr std::uint64_t kAfterFrames[] = {4, 12, 40, 120, 400};
    constexpr std::uint32_t kSteps = sizeof(kAfterFrames) / sizeof(kAfterFrames[0]);
    RecheckState& state = recheckStep_[texture];
    state.step = restart ? 0 : state.step + 1;
    if (state.step >= kSteps) {
        recheckStep_.erase(texture);
        return;
    }
    if (state.queued) return;
    state.queued = true;
    recheckDue_.emplace(frame_ + (nextFrame ? 1 : kAfterFrames[state.step]), texture);
}

void TitleTextures::RunRechecks() {
    constexpr std::size_t kPerFrame = 512;
    constexpr auto kBudget = std::chrono::microseconds(3000);
    const auto started = std::chrono::steady_clock::now();
    std::size_t done = 0;
    while (!recheckDue_.empty() && recheckDue_.begin()->first <= frame_ && done < kPerFrame) {
        if (done != 0 && std::chrono::steady_clock::now() - started > kBudget) break;
        Resource* const texture = recheckDue_.begin()->second;
        recheckDue_.erase(recheckDue_.begin());
        ++done;
        const auto tracked = tracked_.find(texture);
        const auto state = recheckStep_.find(texture);
        if (tracked == tracked_.end() || state == recheckStep_.end()) continue;
        state->second.queued = false;
        bool uploaded = false;
        Visit(texture, tracked->second, uploaded);
        if (deferred_) {
            state->second.queued = true;
            recheckDue_.emplace(frame_ + 1, texture);
            break;
        }
        if (!uploaded) {
            const bool live = tracked->second.changedChecks >= kLiveAfterChecks / 2;
            ScheduleRecheck(texture, live, live);
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
    const TimedScope timed(TimedPart::TextureRefresh);
    constexpr auto kBudget = std::chrono::microseconds(2000);
    const auto started = std::chrono::steady_clock::now();
    const std::lock_guard guard(lock_);
    std::size_t refreshed = 0;
    ++frame_;
    refreshing_ = true;
    deferred_ = false;
    frameUploadBytes_ = 0;
    bool uploaded = false;
    static std::uint64_t partMicroseconds[6] = {};
    auto mark = started;
    const auto part = [&mark](int index) {
        const auto now = std::chrono::steady_clock::now();
        partMicroseconds[index] += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - mark).count());
        mark = now;
    };
    FlushLocked();
    part(0);
    for (auto grave = graveyard_.begin(); grave != graveyard_.end();) {
        if (grave->second + 4 > frame_) {
            ++grave;
            continue;
        }
        grave->first->Release();
        grave = graveyard_.erase(grave);
    }
    part(5);
    RunRechecks();
    part(1);
    for (auto& [texture, tracked] : tracked_) {
        if (!tracked.whole || !tracked.readable) continue;
        if (((frame_ + (reinterpret_cast<std::uintptr_t>(texture) >> 6)) & 3) != 0) continue;
        Visit(texture, tracked, uploaded);
        refreshed += uploaded;
        if (deferred_) break;
    }
    part(2);
    std::size_t visited = 0;
    auto it = tracked_.upper_bound(resumeAfter_);
    for (std::size_t steps = 0; steps < tracked_.size(); ++steps) {
        if (it == tracked_.end()) it = tracked_.begin();
        if (it == tracked_.end()) break;
        if (!it->second.whole || !it->second.readable) {
            Visit(it->first, it->second, uploaded);
            refreshed += uploaded;
            ++visited;
            if (deferred_) break;
        }
        resumeAfter_ = it->first;
        ++it;
        if ((visited & 7) == 0 && std::chrono::steady_clock::now() - started > kBudget) break;
    }
    static std::atomic<std::uint64_t> total{0};
    if (refreshed != 0 && total.fetch_add(refreshed) == 0) {
        AbxLog("d3d12_x", "title textures are uploaded again when their memory changes (glyph-cache render targets hashed whole every 4 frames, the rest from 256 samples each, round-robin within 2 ms a frame)");
    }
    part(3);
    refreshing_ = false;
    FlushLocked();
    part(4);
    refreshMicroseconds_ += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    if (frame_ % 300 == 0) {
        AbxLog("d3d12_x", "title texture refresh: %llu textures tracked, %llu uploaded again so far, %llu released for reused memory, %.2f ms a frame", static_cast<unsigned long long>(tracked_.size()),
               static_cast<unsigned long long>(total.load()), static_cast<unsigned long long>(evicted_), static_cast<double>(refreshMicroseconds_) / 300000.0);
        AbxLog("d3d12_x", "DIAG title texture re-uploads deferred to later frames by the %llu MiB per-frame budget: %llu so far, %zu rechecks queued; %llu waits for memory "
                          "that was still changing (a changed texture is uploaded once two checks 4 frames apart agree, or at every check once it has changed at 16 checks in a row, as video frames do)",
               static_cast<unsigned long long>(kRefreshUploadBytes >> 20), static_cast<unsigned long long>(deferredUploads_), recheckDue_.size(),
               static_cast<unsigned long long>(settleWaits_));
        AbxLog("d3d12_x", "DIAG texture visits per frame: %.1f visits (%.1f not mapped yet), access checks %.2f ms, hashing %.2f ms direct + %.2f ms through pages, re-uploads %.2f ms",
               static_cast<double>(visitCount_) / 300.0, static_cast<double>(visitNotMapped_) / 300.0, static_cast<double>(visitAccessUs_) / 300000.0,
               static_cast<double>(visitHashUs_) / 300000.0, static_cast<double>(visitHashIndirectUs_) / 300000.0, static_cast<double>(visitUploadUs_) / 300000.0);
        visitCount_ = visitNotMapped_ = visitAccessUs_ = visitHashUs_ = visitHashIndirectUs_ = visitUploadUs_ = 0;
        AbxLog("d3d12_x", "DIAG title texture refresh parts per frame: first flush %.2f ms, graveyard releases %.2f ms, rechecks %.2f ms, whole-hashed textures %.2f ms, sampled round-robin %.2f ms, last flush %.2f ms",
               static_cast<double>(partMicroseconds[0]) / 300000.0, static_cast<double>(partMicroseconds[5]) / 300000.0, static_cast<double>(partMicroseconds[1]) / 300000.0,
               static_cast<double>(partMicroseconds[2]) / 300000.0,
               static_cast<double>(partMicroseconds[3]) / 300000.0, static_cast<double>(partMicroseconds[4]) / 300000.0);
        for (std::uint64_t& value : partMicroseconds) value = 0;
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
        if (filled == Filled::Yes) {
            ++tracked.uploads;
            tracked.lastUpload = frame_;
        }
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            AbxLog("d3d12_x", "title textures are uploaded again when DirectStorage writes their memory (first: 0x%llx, %llu bytes)%s%s",
                   static_cast<unsigned long long>(tracked.address), static_cast<unsigned long long>(tracked.bytes), filled == Filled::Yes ? "" : "; that upload failed: ",
                   filled == Filled::Yes ? "" : error.c_str());
        }
    }
}

TitleTextures::Filled TitleTextures::Upload(Resource& texture, std::uint64_t address, std::uint32_t mips, std::string& error, bool filledBefore) {
    texture.NoteUse();
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
    std::uint64_t firstByte = 0;
    std::uint64_t readAt = address;
    std::uint64_t readBytes = titleBytes;
    if (console != nullptr) {
        const std::uint64_t texelBytes = (std::min)(console->texelBytes, console->size - console->texelOffset);
        std::uint64_t first = texelBytes;
        std::uint64_t end = 0;
        bool known = true;
        for (UINT subresource = 0; subresource < count && known; ++subresource) {
            const std::optional<std::pair<std::size_t, std::size_t>> range = gfx10::SubresourceBytes(*console, subresource % mips + skip, subresource / mips);
            known = range.has_value();
            if (!known) break;
            first = (std::min)(first, static_cast<std::uint64_t>(range->first));
            end = (std::max)(end, static_cast<std::uint64_t>(range->second));
        }
        if (!known) {
            first = 0;
            end = texelBytes;
        }
        end = (std::min)(end, texelBytes);
        first = (std::min)(first, end);
        firstByte = first;
        readAt = address + console->texelOffset + first;
        readBytes = end - first;
    }
    struct ScratchTrim {
        std::vector<std::uint8_t>& bytes;
        ~ScratchTrim() {
            if (bytes.capacity() > (64u << 20)) std::vector<std::uint8_t>().swap(bytes);
        }
    } scratchTrim{scratch_};
    std::vector<std::uint8_t>& gathered = scratch_;
    const auto elapsed = [](std::chrono::steady_clock::time_point since) {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since).count());
    };
    const auto readStart = std::chrono::steady_clock::now();
    const bool direct = Readable(readAt, readBytes);
    if (!direct && !Gather(readAt, readBytes, gathered)) {
        error = "the texture's GPU-only title memory could not be read";
        return Filled::NotMapped;
    }
    const bool snapshot = direct && (console != nullptr || !context_.addresses || !context_.addresses->Locate(address, footprints->totalBytes));
    if (!direct || snapshot) uploadParts_.readBytes += readBytes;
    if (snapshot) {
        gathered.resize(static_cast<std::size_t>(readBytes));
        SIZE_T read = 0;
        if (readBytes != 0 && (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(readAt), gathered.data(), gathered.size(), &read) || read != gathered.size())) {
            static std::atomic<int> reported{0};
            if (reported.fetch_add(1) < 4) {
                AbxLog("d3d12_x", "a title texture's memory at 0x%llx went away while it was being uploaded (the title released it); the upload is dropped",
                       static_cast<unsigned long long>(address));
            }
            error = "the texture's title memory went away during the upload";
            return Filled::NotMapped;
        }
    }
    uploadParts_.readUs += elapsed(readStart);
    const std::uint64_t source = (direct && !snapshot) ? address : reinterpret_cast<std::uint64_t>(gathered.data());
    const bool fill = FloatFilled(texture);
    const auto fillFloats = [&](std::uint8_t* linear) {
        for (UINT subresource = 0; subresource < count; ++subresource) {
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = footprints->layouts[subresource];
            for (std::size_t row = 0; row < footprints->rows[subresource]; ++row) {
                std::uint8_t* const out = linear + footprint.Offset + row * footprint.Footprint.RowPitch;
                for (std::size_t at = 0; at + 4 <= static_cast<std::size_t>(footprints->rowBytes[subresource]); at += 4) std::memcpy(out + at, &floatFill_.value, 4);
            }
        }
    };
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
            const auto ownStart = std::chrono::steady_clock::now();
            if (!CreateHostBuffer(context_.device, footprints->totalBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hostStaging, error)) return Filled::Failed;
            uploadParts_.stagingUs += elapsed(ownStart);
            ++uploadParts_.ownStaging;
            linear = static_cast<std::uint8_t*>(hostStaging.mapped);
            staging = {hostStaging.buffer, 0, footprints->totalBytes};
        }
        const auto* texels = reinterpret_cast<const std::uint8_t*>(source);
        const std::size_t texelBytes = static_cast<std::size_t>(readBytes);
        const auto detileStart = std::chrono::steady_clock::now();
        bool detiled = true;
        for (UINT subresource = 0; subresource < count && detiled; ++subresource) {
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = footprints->layouts[subresource];
            detiled = gfx10::DetileSubresource(*console, texels, texelBytes, subresource % mips + skip, subresource / mips, linear + footprint.Offset, footprint.Footprint.RowPitch,
                                               footprints->rows[subresource], static_cast<std::size_t>(footprints->rowBytes[subresource]), error, static_cast<std::size_t>(firstByte));
        }
        uploadParts_.detileUs += elapsed(detileStart);
        if (!detiled) {
            if (offset) context_.arena.Free(*offset);
            if (hostStaging.buffer != VK_NULL_HANDLE) DestroyBuffer(context_.device, hostStaging);
            return Filled::Failed;
        }
        if (fill) fillFloats(linear);
    } else if (context_.addresses != nullptr && !fill) {
        imported = context_.addresses->Locate(address, footprints->totalBytes);
    }
    if (console == nullptr && imported) {
        staging = {imported->buffer, imported->offset, footprints->totalBytes};
    } else if (console == nullptr) {
        offset = StagingLocked(footprints->totalBytes);
        std::uint8_t* linear = nullptr;
        if (offset) {
            linear = context_.window.Base() + *offset;
            staging = {context_.window.Buffer(), *offset, footprints->totalBytes};
        } else {
            const auto ownStart = std::chrono::steady_clock::now();
            if (!CreateHostBuffer(context_.device, footprints->totalBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hostStaging, error)) return Filled::Failed;
            uploadParts_.stagingUs += elapsed(ownStart);
            ++uploadParts_.ownStaging;
            linear = static_cast<std::uint8_t*>(hostStaging.mapped);
            staging = {hostStaging.buffer, 0, footprints->totalBytes};
        }
        const auto copyStart = std::chrono::steady_clock::now();
        std::memcpy(linear, reinterpret_cast<const void*>(source), static_cast<std::size_t>(footprints->totalBytes));
        uploadParts_.detileUs += elapsed(copyStart);
        if (fill) fillFloats(linear);
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
    if (refreshing_) frameUploadBytes_ += footprints->totalBytes;
    if (batchBytes_ > (256ull << 20)) FlushLocked();
    return Filled::Yes;
}

std::optional<std::uint64_t> TitleTextures::StagingLocked(std::uint64_t bytes) {
    const auto started = std::chrono::steady_clock::now();
    std::optional<std::uint64_t> offset = context_.arena.Allocate(bytes, kBufferPlacementAlignment);
    if (!offset && !inFlight_.empty()) {
        ReapLocked(false);
        offset = context_.arena.Allocate(bytes, kBufferPlacementAlignment);
    }
    if (!offset && !liveCheck_ && !inFlight_.empty()) {
        ++uploadParts_.stagingWaits;
        ReapLocked(true);
        offset = context_.arena.Allocate(bytes, kBufferPlacementAlignment);
    }
    uploadParts_.stagingUs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
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

void TitleTextures::RefreshLiveLocked() {
    if (live_.empty()) return;
    const auto started = std::chrono::steady_clock::now();
    if (started < liveNext_) return;
    static const unsigned share = [] {
        const char* value = AbxSetting("d3d12_x.video_upload_percent");
        const unsigned long parsed = value != nullptr ? std::strtoul(value, nullptr, 10) : 0;
        return parsed >= 1 && parsed <= 100 ? static_cast<unsigned>(parsed) : 25u;
    }();
    const std::uint64_t bytesBefore = batchBytes_;
    const UploadParts partsBefore = uploadParts_;
    std::uint64_t texturesUploaded = 0;
    bool any = false;
    liveCheck_ = true;
    for (auto entry = live_.begin(); entry != live_.end();) {
        Resource* const texture = *entry;
        const auto tracked = tracked_.find(texture);
        if (tracked == tracked_.end() || tracked->second.changedChecks < kLiveAfterChecks / 2) {
            entry = live_.erase(entry);
            continue;
        }
        bool uploaded = false;
        Visit(texture, tracked->second, uploaded);
        any = any || uploaded;
        texturesUploaded += uploaded ? 1 : 0;
        if (uploaded && ++liveUploads_ == 1) {
            AbxLog("d3d12_x", "title textures that change at every check (video frames) are also checked just before each ExecuteCommandLists, so the frame the title "
                              "is about to draw is the one uploaded, not a frame it was still writing at the last present");
        }
        ++entry;
    }
    liveCheck_ = false;
    if (!any) return;
    const auto finished = std::chrono::steady_clock::now();
    const auto cost = std::chrono::duration_cast<std::chrono::microseconds>(finished - started);
    liveNext_ = finished + cost * (100 - share) / share;
    liveGroupUs_ += static_cast<std::uint64_t>(cost.count());
    const std::uint64_t groupBytes = batchBytes_ >= bytesBefore ? batchBytes_ - bytesBefore : 0;
    liveGroupBytes_ += groupBytes;
    liveParts_.readUs += uploadParts_.readUs - partsBefore.readUs;
    liveParts_.readBytes += uploadParts_.readBytes - partsBefore.readBytes;
    liveParts_.detileUs += uploadParts_.detileUs - partsBefore.detileUs;
    liveParts_.stagingUs += uploadParts_.stagingUs - partsBefore.stagingUs;
    liveParts_.stagingWaits += uploadParts_.stagingWaits - partsBefore.stagingWaits;
    liveParts_.ownStaging += uploadParts_.ownStaging - partsBefore.ownStaging;
    ++livePartGroups_;
    livePartUs_ += static_cast<std::uint64_t>(cost.count());
    livePartBytes_ += groupBytes;
    livePartTextures_ += texturesUploaded;
    const std::uint64_t groups = ++liveGroups_;
    if (groups == 1 || (groups & (groups - 1)) == 0) {
        const double since = static_cast<double>(livePartGroups_);
        AbxLog("d3d12_x", "DIAG video-like textures: %zu tracked, %llu group uploads so far, %.1f ms and %.1f MiB a group on average; uploads take at most %u%% of the time "
                          "(d3d12_x.video_upload_percent), so the next one waits %.0f ms; the %llu group(s) since the last line: %.1f ms a group for %.1f texture(s), %.2f MiB "
                          "uploaded and %.2f MiB read from title memory (reading %.1f ms, detiling or copying %.1f ms, staging %.1f ms with %llu wait(s) for the GPU and %llu "
                          "staging buffer(s) of their own)",
               live_.size(), static_cast<unsigned long long>(groups), static_cast<double>(liveGroupUs_) / 1000.0 / static_cast<double>(groups),
               static_cast<double>(liveGroupBytes_) / 1048576.0 / static_cast<double>(groups), share, static_cast<double>(cost.count()) * (100 - share) / share / 1000.0,
               static_cast<unsigned long long>(livePartGroups_), static_cast<double>(livePartUs_) / 1000.0 / since, static_cast<double>(livePartTextures_) / since,
               static_cast<double>(livePartBytes_) / 1048576.0 / since, static_cast<double>(liveParts_.readBytes) / 1048576.0 / since,
               static_cast<double>(liveParts_.readUs) / 1000.0 / since, static_cast<double>(liveParts_.detileUs) / 1000.0 / since,
               static_cast<double>(liveParts_.stagingUs) / 1000.0 / since, static_cast<unsigned long long>(liveParts_.stagingWaits),
               static_cast<unsigned long long>(liveParts_.ownStaging));
        liveParts_ = {};
        livePartGroups_ = 0;
        livePartUs_ = 0;
        livePartBytes_ = 0;
        livePartTextures_ = 0;
    }
}

void TitleTextures::Flush() {
    const TimedScope timed(TimedPart::TextureFlush);
    const std::lock_guard guard(lock_);
    RefreshLiveLocked();
    FlushLocked();
}

}
