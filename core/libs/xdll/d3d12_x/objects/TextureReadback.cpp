#include "TextureReadback.hpp"

#include "FrameTiming.hpp"

#include "AbxApi.hpp"
#include "CopyLayout.hpp"
#include "Gfx10Tiling.hpp"
#include "TitleAddressSpace.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

namespace abx::d3d12x {

namespace {

constexpr std::uint64_t kTitlePage = 0x10000;

std::string WordsText(const std::uint8_t* bytes, std::size_t count) {
    std::string words;
    for (std::size_t at = 0; at + 4 <= count && at < 16; at += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, bytes + at, 4);
        char item[16];
        std::snprintf(item, sizeof(item), "%s%08x", at != 0 ? " " : "", word);
        words += item;
    }
    if (words.empty()) {
        for (std::size_t at = 0; at < count; ++at) {
            char item[8];
            std::snprintf(item, sizeof(item), "%s%02x", at != 0 ? " " : "", bytes[at]);
            words += item;
        }
    }
    return words;
}

}

TextureReadback::TextureReadback(const ResourceContext& context) : context_(context) {
    if (const char* value = AbxSetting("d3d12_x.texture_readback_texels")) {
        maxTexels_ = (std::min)(static_cast<std::uint64_t>(std::strtoull(value, nullptr, 10)), kLargestTexels);
    }
    if (maxTexels_ == 0) {
        AbxLog("d3d12_x", "small GPU-written textures are not copied back to title memory (d3d12_x.texture_readback_texels = 0): a title whose CPU reads one there sees the "
                          "memory as it was");
    }
    if (const char* value = AbxSetting("d3d12_x.texture_readback_thread")) ownThread_ = value[0] == '1' && value[1] == 0;
    if (maxTexels_ != 0) {
        AbxLog("d3d12_x", "small GPU-written textures are copied back to title memory %s (d3d12_x.texture_readback_thread)",
               ownThread_ ? "by a thread of their own, a few milliseconds after the GPU wrote them, as the console's GPU writes that memory whenever it gets there"
                          : "on the title's thread before each ExecuteCommandLists and present");
    }
    if (ownThread_ && maxTexels_ != 0) thread_ = std::thread([this] { RunPasses(); });
    active_.store(this);
}

TextureReadback::~TextureReadback() {
    active_.store(nullptr);
    if (thread_.joinable()) {
        {
            const std::lock_guard wake(wakeLock_);
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }
    const std::lock_guard pass(passLock_);
    const std::lock_guard guard(lock_);
    for (auto& [texture, slot] : slots_) {
        texture->SetReadBack(false);
        DestroyBuffer(context_.device, slot.staging);
    }
    for (auto& [buffer, stamp] : retired_) DestroyBuffer(context_.device, buffer);
}

void TextureReadback::Consider(Resource& texture, std::uint64_t address, const D3D12_RESOURCE_DESC& desc) {
    constexpr UINT kWritten = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const UINT flags = static_cast<UINT>(desc.Flags);
    if (maxTexels_ == 0 || address == 0 || texture.IsBuffer()) return;
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || (flags & kWritten) == 0 || (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0 || desc.SampleDesc.Count > 1 ||
        desc.DepthOrArraySize != 1) {
        return;
    }
    if (desc.Width == 0 || desc.Height == 0 || desc.Width > maxTexels_ || desc.Height > maxTexels_ || desc.Width * desc.Height > maxTexels_) return;
    const TextureRef& image = texture.Texture();
    if (image.layout.info.mipLevels != 1) return;
    const std::optional<CopyBlock> block = CopyBlockOf(image.desc.Format);
    if (!block || block->width != 1 || block->height != 1 || block->bytes == 0) return;
    const std::lock_guard guard(lock_);
    if (slots_.count(&texture) != 0) return;
    char name[96];
    std::snprintf(name, sizeof(name), "%llux%u, format %d, flags 0x%x", static_cast<unsigned long long>(desc.Width), desc.Height, static_cast<int>(desc.Format), flags);
    const auto refuse = [&](const std::string& why) {
        if (refusals_.insert(why).second) {
            AbxLog("d3d12_x", "a small GPU-written texture at title address 0x%llx (%s) is not copied back to title memory (later cases of this reason are not logged): %s",
                   static_cast<unsigned long long>(address), name, why.c_str());
        }
    };
    if (slots_.size() >= kMaxTextures) {
        if (!full_) {
            full_ = true;
            AbxLog("d3d12_x", "%zu small GPU-written textures are already copied back to title memory; later ones (first: 0x%llx, %s) are not until one of those is released",
                   slots_.size(), static_cast<unsigned long long>(address), name);
        }
        return;
    }
    std::string why;
    const std::optional<gfx10::ConsoleTexture> console = gfx10::DescribeConsoleTexture(desc, gfx10::ConsolePipes(AbxSetting("runtime.console_model")), why);
    if (!console) return refuse("the console layout is not described: " + why);
    if (console->mips != 1 || console->depth != 0 || console->chain.levels.empty() || console->size <= console->texelOffset) {
        return refuse("the console layout has " + std::to_string(console->mips) + " mips, depth " + std::to_string(console->depth) + " and " + std::to_string(console->size) +
                      " bytes, which is not one 2D level with texels");
    }
    const std::optional<Footprints> footprints = DesktopCopyableFootprints(image.desc, 1, 0, 1, 0, why);
    if (!footprints || footprints->layouts.empty()) return refuse("the copy footprint is not known: " + why);
    const gfx10::MipLevel& level = console->chain.levels[0];
    const std::size_t element = std::size_t{1} << console->chain.layout->elementLog2;
    if (level.width * element != footprints->rowBytes[0] || level.height != footprints->rows[0] || element != block->bytes) {
        return refuse("the console level is " + std::to_string(level.width) + "x" + std::to_string(level.height) + " elements of " + std::to_string(element) +
                      " bytes but the copy footprint has " + std::to_string(footprints->rowBytes[0]) + " bytes in " + std::to_string(footprints->rows[0]) + " rows");
    }
    const std::uint64_t texelBytes = (std::min)(console->texelBytes, console->size - console->texelOffset);
    Slot slot;
    slot.address = address;
    slot.texelBase = address + console->texelOffset;
    slot.footprint = footprints->layouts[0];
    slot.copyBytes = footprints->totalBytes;
    slot.sequenceOffset = (footprints->totalBytes + 15) & ~std::uint64_t{15};
    slot.element = static_cast<std::uint32_t>(element);
    slot.mode = console->mode;
    slot.name = name;
    slot.id = ++slotIds_;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> elements;
    elements.reserve(level.width * level.height);
    for (std::size_t y = 0; y < level.height; ++y) {
        for (std::size_t x = 0; x < level.width; ++x) {
            const std::size_t at = console->chain.ElementAt(level, x, y, 0);
            if (at + element > texelBytes) return refuse("a texel lies outside the console layout's texel bytes");
            elements.emplace_back(at, static_cast<std::uint32_t>(slot.footprint.Offset + y * slot.footprint.Footprint.RowPitch + x * element));
        }
    }
    std::sort(elements.begin(), elements.end());
    if (elements.empty()) return refuse("the console level has no texels");
    slot.elements = std::make_shared<const std::vector<std::pair<std::uint64_t, std::uint32_t>>>(std::move(elements));
    if (!CreateHostBuffer(context_.device, slot.sequenceOffset + 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, slot.staging, why)) return refuse("no staging buffer: " + why);
    std::memset(slot.staging.mapped, 0, static_cast<std::size_t>(slot.sequenceOffset + 16));
    const std::uint64_t first = slot.texelBase + slot.elements->front().first;
    slots_.emplace(&texture, std::move(slot));
    texture.SetReadBack(true);
    const std::uint64_t count = ++considered_;
    if (count <= 32 || (count & (count - 1)) == 0) {
        AbxLog("d3d12_x", "a small GPU-written texture at title address 0x%llx (%s, console tiling mode %u) is copied back to title memory after each pass that writes it, "
                          "as it is one memory on the console and the title's CPU may read it: its first texel goes to 0x%llx (%zu alive, at most %zu, %llu so far; "
                          "d3d12_x.texture_readback_texels sets the largest texel count, now %llu, and 0 turns this off)",
               static_cast<unsigned long long>(address), name, console->mode, static_cast<unsigned long long>(first), slots_.size(), kMaxTextures,
               static_cast<unsigned long long>(count), static_cast<unsigned long long>(maxTexels_));
    }
}

void TextureReadback::Forget(Resource* texture) {
    const std::lock_guard guard(lock_);
    const auto found = slots_.find(texture);
    if (found == slots_.end()) return;
    retired_.emplace_back(found->second.staging, presents_);
    slots_.erase(found);
    full_ = false;
}

bool TextureReadback::Record(Resource& texture, D3D12_RESOURCE_STATES state, VkCommandBuffer commands, std::string& error) {
    VkBuffer buffer = VK_NULL_HANDLE;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    std::uint64_t copyBytes = 0;
    std::uint64_t sequenceOffset = 0;
    std::uint32_t sequence = 0;
    {
        const std::lock_guard guard(lock_);
        const auto found = slots_.find(&texture);
        if (found == slots_.end()) return true;
        Slot& slot = found->second;
        if (++slot.recorded == 0) ++slot.recorded;
        sequence = slot.recorded;
        buffer = slot.staging.buffer;
        footprint = slot.footprint;
        copyBytes = slot.copyBytes;
        sequenceOffset = slot.sequenceOffset;
    }
    CommandRecorder recorder(context_.device, commands);
    const bool direct = state == D3D12_RESOURCE_STATE_COMMON || (static_cast<UINT>(state) & D3D12_RESOURCE_STATE_COPY_SOURCE) != 0;
    if (!direct && !recorder.Transition(texture.Texture(), state, D3D12_RESOURCE_STATE_COPY_SOURCE, error)) return false;
    bool copied = recorder.CopyTextureToBuffer(BufferRef{buffer, 0, copyBytes}, footprint, texture.Texture(), direct ? state : D3D12_RESOURCE_STATE_COPY_SOURCE, 0, nullptr, error);
    if (!direct) {
        std::string back;
        if (!recorder.Transition(texture.Texture(), D3D12_RESOURCE_STATE_COPY_SOURCE, state, back)) {
            error = back;
            copied = false;
        }
    }
    if (copied) context_.device.Vk().cmdFillBuffer(commands, buffer, sequenceOffset, 4, sequence);
    return copied;
}

bool TextureReadback::WriteTitle(std::uint64_t address, const std::uint8_t* data, std::size_t bytes, DWORD& failure) const {
    std::size_t done = 0;
    while (done < bytes) {
        const std::uint64_t at = address + done;
        const std::size_t piece = static_cast<std::size_t>((std::min)(kTitlePage - (at & (kTitlePage - 1)), static_cast<std::uint64_t>(bytes - done)));
        const std::uint8_t* host = context_.addresses != nullptr ? context_.addresses->HostAt(at) : nullptr;
        void* const target = host != nullptr ? const_cast<std::uint8_t*>(host) : reinterpret_cast<void*>(at);
        SIZE_T written = 0;
        const TimedScope timed(TimedPart::ReadbackWrite);
        if (!WriteProcessMemory(GetCurrentProcess(), target, data + done, piece, &written) || written != piece) {
            failure = GetLastError();
            return false;
        }
        done += piece;
    }
    return true;
}

bool TextureReadback::ReadTitle(std::uint64_t address, std::uint8_t* data, std::size_t bytes) const {
    std::size_t done = 0;
    while (done < bytes) {
        const std::uint64_t at = address + done;
        const std::size_t piece = static_cast<std::size_t>((std::min)(kTitlePage - (at & (kTitlePage - 1)), static_cast<std::uint64_t>(bytes - done)));
        const std::uint8_t* host = context_.addresses != nullptr ? context_.addresses->HostAt(at) : nullptr;
        const void* const source = host != nullptr ? static_cast<const void*>(host) : reinterpret_cast<const void*>(at);
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), source, data + done, piece, &read) || read != piece) return false;
        done += piece;
    }
    return true;
}

void TextureReadback::WriteCopy(Copy& copy) {
    const std::vector<std::pair<std::uint64_t, std::uint32_t>>& elements = *copy.elements;
    const std::uint64_t firstAt = copy.texelBase + elements.front().first;
    const std::size_t shown = (std::min)(static_cast<std::size_t>(copy.element), sizeof(copy.before));
    copy.readBefore = copy.first && ReadTitle(firstAt, copy.before, shown);
    std::size_t index = 0;
    copy.written = true;
    while (index < elements.size() && copy.written) {
        const std::uint64_t start = elements[index].first;
        std::uint64_t end = start;
        run_.clear();
        while (index < elements.size() && elements[index].first == end) {
            const std::uint8_t* const source = copy.texels.data() + elements[index].second;
            run_.insert(run_.end(), source, source + copy.element);
            end += copy.element;
            ++index;
        }
        copy.written = WriteTitle(copy.texelBase + start, run_.data(), run_.size(), copy.failure);
        if (!copy.written) {
            copy.failedAt = copy.texelBase + start;
            copy.failedBytes = run_.size();
        }
        ++copy.runs;
    }
}

void TextureReadback::Pass() {
    const std::lock_guard pass(passLock_);
    copies_.clear();
    {
        const std::lock_guard guard(lock_);
        for (auto& [texture, slot] : slots_) {
            const auto* mapped = static_cast<const std::uint8_t*>(slot.staging.mapped);
            std::uint32_t sequence = 0;
            std::memcpy(&sequence, mapped + slot.sequenceOffset, 4);
            if (sequence == slot.delivered || presents_ < slot.retryAt) continue;
            Copy copy;
            copy.texture = texture;
            copy.id = slot.id;
            copy.sequence = sequence;
            copy.texelBase = slot.texelBase;
            copy.element = slot.element;
            copy.first = slot.deliveries == 0;
            copy.elements = slot.elements;
            copy.texels.assign(mapped, mapped + slot.copyBytes);
            copies_.push_back(std::move(copy));
        }
    }
    if (copies_.empty()) return;
    for (Copy& copy : copies_) WriteCopy(copy);
    const std::lock_guard guard(lock_);
    for (const Copy& copy : copies_) {
        const auto found = slots_.find(copy.texture);
        if (found == slots_.end() || found->second.id != copy.id) continue;
        Slot& slot = found->second;
        if (!copy.written) {
            slot.retryAt = presents_ + kRetryAfterPresents;
            if (!slot.failureLogged) {
                slot.failureLogged = true;
                AbxLog("d3d12_x", "a small GPU-written texture at title address 0x%llx (%s) could not be copied back to title memory: writing %zu byte(s) at 0x%llx failed with "
                                  "error %lu; tried again every %llu presents",
                       static_cast<unsigned long long>(slot.address), slot.name.c_str(), copy.failedBytes, static_cast<unsigned long long>(copy.failedAt), copy.failure,
                       static_cast<unsigned long long>(kRetryAfterPresents));
            }
            continue;
        }
        slot.delivered = copy.sequence;
        slot.retryAt = 0;
        if (slot.deliveries++ == 0) {
            const std::uint64_t count = ++reported_;
            if (count <= 64 || (count & (count - 1)) == 0) {
                const std::vector<std::pair<std::uint64_t, std::uint32_t>>& elements = *copy.elements;
                const std::size_t shown = (std::min)(static_cast<std::size_t>(copy.element), sizeof(copy.before));
                const std::uint8_t* const firstTexel = copy.texels.data() + elements.front().second;
                AbxLog("d3d12_x", "texture readback: the GPU's texels of the texture at title address 0x%llx (%s) are now in title memory, %zu byte(s) in %zu run(s) from 0x%llx; "
                                  "the first texel was %s there and is %s on the GPU; later copies of this texture are not logged (%llu texture(s) so far)",
                       static_cast<unsigned long long>(slot.address), slot.name.c_str(), elements.size() * static_cast<std::size_t>(copy.element), copy.runs,
                       static_cast<unsigned long long>(copy.texelBase + elements.front().first), copy.readBefore ? WordsText(copy.before, shown).c_str() : "not readable",
                       WordsText(firstTexel, shown).c_str(), static_cast<unsigned long long>(count));
            }
        }
        const std::uint64_t count = ++delivered_;
        if (count >= 64 && (count & (count - 1)) == 0) {
            AbxLog("d3d12_x", "texture readback: %llu copies of small GPU-written textures delivered to title memory so far, %zu texture(s) alive",
                   static_cast<unsigned long long>(count), slots_.size());
        }
    }
}

void TextureReadback::RunPasses() {
    constexpr auto kBusy = std::chrono::milliseconds(4);
    constexpr auto kIdle = std::chrono::milliseconds(100);
    for (;;) {
        bool any = false;
        {
            const std::lock_guard guard(lock_);
            any = !slots_.empty();
        }
        {
            std::unique_lock wake(wakeLock_);
            wake_.wait_for(wake, any ? kBusy : kIdle);
            if (stop_) return;
        }
        const TimedScope timed(TimedPart::Readback);
        Pass();
    }
}

void TextureReadback::Deliver(bool present) {
    if (present) {
        const std::lock_guard guard(lock_);
        ++presents_;
        for (auto entry = retired_.begin(); entry != retired_.end();) {
            if (entry->second + kRetireAfterPresents > presents_) {
                ++entry;
                continue;
            }
            DestroyBuffer(context_.device, entry->first);
            entry = retired_.erase(entry);
        }
    }
    if (ownThread_) {
        wake_.notify_one();
        return;
    }
    const TimedScope timed(TimedPart::Readback);
    Pass();
}

}
