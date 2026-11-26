#include "Gfx10Tiling.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <tuple>

namespace abx::d3d12x::gfx10 {

namespace {

struct PatternRows {
    std::uint32_t mode;
    const std::uint16_t (*rows)[4];
    std::size_t count;
};

#include "Gfx10SwizzleTables.inc"

std::size_t DivCeil(std::size_t value, std::size_t divisor) {
    return (value + divisor - 1) / divisor;
}

std::uint64_t DivCeil64(std::uint64_t value, std::uint64_t divisor) {
    return (value + divisor - 1) / divisor;
}

bool IsXor(std::uint32_t mode) {
    return mode >= 16;
}

std::optional<std::uint32_t> BlockLog2(std::uint32_t mode) {
    switch (mode) {
    case 1:
    case 2: return 8u;
    case 5:
    case 6:
    case 21:
    case 22: return 12u;
    case 9:
    case 10:
    case 17:
    case 18:
    case 24:
    case 25:
    case 26:
    case 27: return 16u;
    default: return std::nullopt;
    }
}

std::size_t PipeGroup(PipeConfig pipes) {
    return pipes.pipesLog2 + (pipes.packersLog2 >= 2 ? 2 * pipes.packersLog2 - 2 : 0);
}

template <std::size_t N>
std::optional<std::array<std::uint64_t, 20>> PatternBits(const PatternRows (&table)[N], std::uint32_t mode, std::uint32_t elementLog2, PipeConfig pipes) {
    const PatternRows* found = nullptr;
    for (const PatternRows& entry : table) {
        if (entry.mode == mode) found = &entry;
    }
    if (found == nullptr) return std::nullopt;
    const std::size_t row = (IsXor(mode) ? PipeGroup(pipes) * 5 : 0) + elementLog2;
    if (row >= found->count) return std::nullopt;
    const std::uint16_t* index = found->rows[row];
    if (index[0] >= std::size(kNibble01) || index[1] >= std::size(kNibble2) || index[2] >= std::size(kNibble3) || index[3] >= std::size(kNibble4)) return std::nullopt;
    std::array<std::uint64_t, 20> bits{};
    std::copy(std::begin(kNibble01[index[0]]), std::end(kNibble01[index[0]]), bits.begin());
    std::copy(std::begin(kNibble2[index[1]]), std::end(kNibble2[index[1]]), bits.begin() + 8);
    std::copy(std::begin(kNibble3[index[2]]), std::end(kNibble3[index[2]]), bits.begin() + 12);
    std::copy(std::begin(kNibble4[index[3]]), std::end(kNibble4[index[3]]), bits.begin() + 16);
    return bits;
}

std::vector<std::uint32_t> OffsetTable(const std::array<std::uint64_t, 20>& bits, std::uint32_t blockLog2, std::size_t count, std::uint32_t shift, std::uint32_t low) {
    std::vector<std::uint32_t> table(count);
    for (std::size_t coordinate = 0; coordinate < count; ++coordinate) {
        const std::uint32_t value = static_cast<std::uint32_t>(coordinate) << low;
        std::uint32_t offset = 0;
        for (std::uint32_t bit = 0; bit < blockLog2; ++bit) {
            const std::uint32_t mask = static_cast<std::uint32_t>((bits[bit] >> shift) & 0xFFFFu);
            offset |= (static_cast<std::uint32_t>(std::popcount(mask & value)) & 1u) << bit;
        }
        table[coordinate] = offset;
    }
    return table;
}

Layout FromBits(std::uint32_t mode, std::uint32_t elementLog2, std::uint32_t blockLog2, const std::array<std::uint64_t, 20>& bits, std::uint32_t wl, std::uint32_t hl, std::uint32_t dl) {
    const auto high = [&](std::uint32_t level, std::uint32_t shift) { return OffsetTable(bits, blockLog2, std::size_t{1} << (16 - (std::min)(level, 16u)), shift, level); };
    Layout layout;
    layout.mode = mode;
    layout.elementLog2 = elementLog2;
    layout.blockWidth = std::size_t{1} << wl;
    layout.blockHeight = std::size_t{1} << hl;
    layout.blockDepth = std::size_t{1} << dl;
    layout.blockBytes = std::size_t{1} << blockLog2;
    layout.ox = OffsetTable(bits, blockLog2, layout.blockWidth, 0, 0);
    layout.oy = OffsetTable(bits, blockLog2, layout.blockHeight, 16, 0);
    layout.oz = OffsetTable(bits, blockLog2, layout.blockDepth, 32, 0);
    layout.hx = high(wl, 0);
    layout.hy = high(hl, 16);
    layout.hz = high(dl, 32);
    return layout;
}

std::optional<Layout> Build2d(std::uint32_t mode, std::uint32_t elementLog2, PipeConfig pipes) {
    if (elementLog2 > 4) return std::nullopt;
    if (mode == 0) {
        Layout layout;
        layout.mode = 0;
        layout.elementLog2 = elementLog2;
        layout.blockWidth = std::size_t{256} >> elementLog2;
        layout.blockBytes = 256;
        layout.ox.resize(layout.blockWidth);
        for (std::size_t x = 0; x < layout.blockWidth; ++x) layout.ox[x] = static_cast<std::uint32_t>(x << elementLog2);
        layout.oy = layout.oz = layout.hx = layout.hy = layout.hz = {0};
        return layout;
    }
    const std::optional<std::uint32_t> block = BlockLog2(mode);
    if (!block) return std::nullopt;
    const std::optional<std::array<std::uint64_t, 20>> bits = PatternBits(kPatterns2d, mode, elementLog2, pipes);
    if (!bits) return std::nullopt;
    constexpr std::uint32_t kWidth256[] = {4, 4, 3, 3, 2};
    constexpr std::uint32_t kHeight256[] = {4, 3, 3, 2, 2};
    const std::uint32_t amplify = *block - 8;
    return FromBits(mode, elementLog2, *block, *bits, kWidth256[elementLog2] + amplify / 2, kHeight256[elementLog2] + amplify - amplify / 2, 0);
}

std::optional<Layout> Build3d(std::uint32_t mode, std::uint32_t elementLog2, PipeConfig pipes) {
    switch (mode) {
    case 0:
    case 24:
    case 27: return Build2d(mode, elementLog2, pipes);
    case 5:
    case 9:
    case 17:
    case 21:
    case 25:
    case 26: break;
    default: return std::nullopt;
    }
    if (elementLog2 > 4) return std::nullopt;
    const std::optional<std::uint32_t> block = BlockLog2(mode);
    if (!block) return std::nullopt;
    const std::optional<std::array<std::uint64_t, 20>> bits = PatternBits(kPatterns3d, mode, elementLog2, pipes);
    if (!bits) return std::nullopt;
    constexpr std::uint32_t kWidth1k[] = {4, 3, 3, 3, 2};
    constexpr std::uint32_t kHeight1k[] = {3, 3, 3, 2, 2};
    constexpr std::uint32_t kDepth1k[] = {3, 3, 2, 2, 2};
    const std::uint32_t average = (*block - 10) / 3;
    const std::uint32_t rest = (*block - 10) % 3;
    return FromBits(mode, elementLog2, *block, *bits, kWidth1k[elementLog2] + average, kHeight1k[elementLog2] + average + rest / 2,
                    kDepth1k[elementLog2] + average + (rest != 0 ? 1u : 0u));
}

std::shared_ptr<const Layout> Cached(bool thick, std::uint32_t mode, std::uint32_t elementLog2, PipeConfig pipes) {
    static std::mutex lock;
    static std::map<std::tuple<bool, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>, std::shared_ptr<const Layout>> cache;
    const auto key = std::make_tuple(thick, mode, elementLog2, pipes.pipesLog2, pipes.packersLog2);
    const std::lock_guard guard(lock);
    const auto found = cache.find(key);
    if (found != cache.end()) return found->second;
    std::optional<Layout> built = thick ? Build3d(mode, elementLog2, pipes) : Build2d(mode, elementLog2, pipes);
    std::shared_ptr<const Layout> shared = built ? std::make_shared<const Layout>(std::move(*built)) : nullptr;
    cache.emplace(key, shared);
    return shared;
}

std::uint32_t FullMips(std::uint64_t largest) {
    return largest == 0 ? 1u : static_cast<std::uint32_t>(std::bit_width(largest));
}

std::size_t FirstTail(const Chain& chain) {
    for (std::size_t i = 0; i < chain.levels.size(); ++i) {
        if (chain.levels[i].tail) return i;
    }
    return chain.levels.size();
}

std::pair<std::size_t, std::size_t> MicroBlock(std::uint32_t elementLog2) {
    constexpr std::uint32_t kWidth[] = {4, 4, 3, 3, 2};
    constexpr std::uint32_t kHeight[] = {4, 3, 3, 2, 2};
    return {std::size_t{1} << kWidth[elementLog2], std::size_t{1} << kHeight[elementLog2]};
}

void Store64(std::span<std::uint8_t, kResourceLayoutBytes> layout, std::size_t offset, std::uint64_t value) {
    std::memcpy(layout.data() + offset, &value, sizeof(value));
}

void Store32(std::span<std::uint8_t, kResourceLayoutBytes> layout, std::size_t offset, std::uint32_t value) {
    std::memcpy(layout.data() + offset, &value, sizeof(value));
}

std::optional<Surface> MakeSurface(std::uint32_t mode, std::uint32_t elementLog2, std::uint32_t side, std::uint32_t width, std::uint32_t height, std::uint32_t array, std::uint32_t mips,
                                   PipeConfig pipes, Refusal& refusal) {
    const bool blockCompressed = side > 1;
    switch (mode) {
    case 0:
    case 1:
    case 2:
    case 5:
    case 6:
    case 9:
    case 10:
    case 17:
    case 18:
    case 21:
    case 22:
    case 25:
    case 26:
    case 27: break;
    case 24:
        if (elementLog2 <= 3 && !blockCompressed) break;
        refusal = {E_INVALIDARG, "swizzle mode 24 (64KB_Z_X) with " + std::to_string(1u << elementLog2) + "-byte or block-compressed elements is not a mode the console's layout library takes"};
        return std::nullopt;
    case 28:
    case 31: refusal = {E_NOTIMPL, "swizzle mode " + std::to_string(mode) + " (VAR) has no layout model"}; return std::nullopt;
    default: refusal = {E_INVALIDARG, "swizzle mode " + std::to_string(mode) + " is not one the console's layout library takes for a 2D texture"}; return std::nullopt;
    }
    std::shared_ptr<const Layout> layout = Layout2d(mode, elementLog2, pipes);
    if (!layout) {
        refusal = {E_INVALIDARG, "swizzle mode " + std::to_string(mode) + " has no pattern for " + std::to_string(1u << elementLog2) + "-byte elements"};
        return std::nullopt;
    }
    Surface surface;
    surface.chain = Chain::Make(std::move(layout), width, height, 1, side, mips);
    surface.mode = mode;
    surface.bytesPerElement = 1u << elementLog2;
    surface.side = side;
    surface.width = width;
    surface.height = height;
    surface.array = array;
    surface.depth = 0;
    surface.firstTail = FirstTail(surface.chain);
    return surface;
}

std::optional<ConsoleLayout> Volume(std::uint64_t width, std::uint32_t height, std::uint32_t depth, std::uint32_t mips, std::uint32_t format, std::uint32_t layoutValue, std::uint32_t flags,
                                    PipeConfig pipes, Refusal& refusal) {
    if (width == 0 || width > 2048 || height == 0 || height > 2048 || depth == 0 || depth > 2048) {
        refusal = {E_INVALIDARG, "a 3D texture outside 1..2048 in some dimension"};
        return std::nullopt;
    }
    if ((flags & 3u) != 0) {
        refusal = {E_NOTIMPL, "a 3D render target or depth buffer has no layout model"};
        return std::nullopt;
    }
    const std::optional<std::pair<std::uint32_t, std::uint32_t>> element = FormatElement(format);
    if (!element) {
        refusal = {E_INVALIDARG, "format " + std::to_string(format) + " has no tiled element size"};
        return std::nullopt;
    }
    const auto [elementLog2, side] = *element;
    std::uint32_t mode = 0;
    if (layoutValue == 1) {
        mode = 0;
    } else if (layoutValue >= 0x100 && layoutValue <= 0x11F) {
        mode = layoutValue & 0x1Fu;
    } else if (layoutValue == 0 && (flags & 4u) != 0) {
        mode = OptimalMode(0, flags, (std::uint64_t{8} << elementLog2) / (side * side) * depth, width, height);
    } else if (layoutValue == 0) {
        mode = 5;
    } else if (layoutValue == 2) {
        refusal = {E_NOTIMPL, "the 64KB_D layout choice of a 3D texture has no layout model"};
        return std::nullopt;
    } else {
        refusal = {E_INVALIDARG, "3D texture layout " + std::to_string(layoutValue) + " is not one the console's library takes"};
        return std::nullopt;
    }
    const std::uint32_t full = FullMips((std::max)({width, static_cast<std::uint64_t>(height), static_cast<std::uint64_t>(depth)}));
    const std::uint32_t levels = mips == 0 ? full : mips;
    if (levels > kMaxMips) {
        refusal = {E_INVALIDARG, std::to_string(levels) + " mips are more than the console's 15"};
        return std::nullopt;
    }
    std::shared_ptr<const Layout> layout = Layout3d(mode, elementLog2, pipes);
    if (!layout) {
        refusal = {E_INVALIDARG, "swizzle mode " + std::to_string(mode) + " is not one a 3D texture can have"};
        return std::nullopt;
    }
    Surface surface;
    surface.chain = Chain::Make(std::move(layout), static_cast<std::uint32_t>(width), height, depth, side, levels);
    surface.mode = mode;
    surface.bytesPerElement = 1u << elementLog2;
    surface.side = side;
    surface.width = static_cast<std::uint32_t>(width);
    surface.height = height;
    surface.array = 1;
    surface.depth = depth;
    surface.firstTail = FirstTail(surface.chain);
    ConsoleLayout result;
    result.planes.push_back(Plane{kUsageColor, 0, std::move(surface)});
    result.main = 0;
    result.mips = levels;
    result.empty = false;
    result.dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    return result;
}

struct SubresourceView {
    const Surface* surface = nullptr;
    std::uint64_t offset = 0;
    std::size_t mip = 0;
    std::size_t slice = 0;
};

std::optional<SubresourceView> FindSubresource(const ConsoleLayout& layout, std::uint32_t plane, std::uint32_t subresource) {
    const std::optional<PlaneView> view = layout.PlaneSurface(plane);
    if (!view) return std::nullopt;
    const std::size_t mips = view->surface->chain.levels.size();
    const std::size_t mip = subresource % mips;
    const std::size_t slice = subresource / mips;
    if (slice >= view->surface->array) return std::nullopt;
    return SubresourceView{view->surface, view->offset, mip, slice};
}

}

std::shared_ptr<const Layout> Layout2d(std::uint32_t mode, std::uint32_t elementLog2, PipeConfig pipes) {
    return Cached(false, mode, elementLog2, pipes);
}

std::shared_ptr<const Layout> Layout3d(std::uint32_t mode, std::uint32_t elementLog2, PipeConfig pipes) {
    return Cached(true, mode, elementLog2, pipes);
}

std::pair<std::size_t, std::size_t> Layout::Blocks(std::size_t width, std::size_t height) const {
    return {DivCeil(width, blockWidth), DivCeil(height, blockHeight)};
}

std::size_t Layout::Size(std::size_t width, std::size_t height) const {
    return DivCeil(width, blockWidth) * DivCeil(height, blockHeight) * blockBytes;
}

std::size_t Layout::Offset(std::size_t x, std::size_t y, std::size_t z, std::size_t width) const {
    const std::size_t pitchBlocks = DivCeil(width, blockWidth);
    const std::size_t block = (y / blockHeight) * pitchBlocks + x / blockWidth;
    const std::uint32_t bits = ox[x % blockWidth] ^ oy[y % blockHeight] ^ oz[z % blockDepth] ^ hx[(x / blockWidth) % hx.size()] ^ hy[(y / blockHeight) % hy.size()] ^
                               hz[(z / blockDepth) % hz.size()];
    return block * blockBytes + (bits ^ baseXor);
}

std::vector<std::uint8_t> Layout::Detile(std::span<const std::uint8_t> tiled, std::size_t width, std::size_t height) const {
    const std::size_t element = std::size_t{1} << elementLog2;
    std::vector<std::uint8_t> out(width * height * element);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const std::size_t at = Offset(x, y, 0, width);
            if (at + element <= tiled.size()) std::memcpy(out.data() + (y * width + x) * element, tiled.data() + at, element);
        }
    }
    return out;
}

bool Layout::Tile(std::span<const std::uint8_t> linear, std::size_t width, std::size_t height, std::vector<std::uint8_t>& tiled) const {
    const std::size_t element = std::size_t{1} << elementLog2;
    if (linear.size() < width * height * element) return false;
    tiled.assign(Size(width, height), 0);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const std::size_t at = Offset(x, y, 0, width);
            if (at + element > tiled.size()) return false;
            std::memcpy(tiled.data() + at, linear.data() + (y * width + x) * element, element);
        }
    }
    return true;
}

Chain Chain::Make(std::shared_ptr<const Layout> layout, std::uint32_t width, std::uint32_t height, std::uint32_t depth, std::uint32_t side, std::uint32_t mips) {
    const std::size_t levelCount = (std::max)(mips, 1u);
    const Layout& l = *layout;
    const std::size_t bw = l.blockWidth;
    const std::size_t bh = l.blockHeight;
    const bool thick = l.blockDepth > 1;
    const std::size_t blockLog2 = static_cast<std::size_t>(std::countr_zero(l.blockBytes));
    const auto size = [&](std::size_t i) {
        return std::make_pair(DivCeil((std::max)(width >> i, 1u), side), DivCeil((std::max)(height >> i, 1u), side));
    };
    const std::size_t w0 = DivCeil(width, side);
    const std::size_t h0 = DivCeil(height, side);
    const auto padded = [&](std::size_t i) { return std::make_pair(DivCeil(w0, std::size_t{1} << i), DivCeil(h0, std::size_t{1} << i)); };
    const auto depthOf = [&](std::size_t i) { return static_cast<std::size_t>((std::max)(depth >> i, 1u)); };
    std::size_t tailWidth = 0;
    std::size_t tailHeight = 0;
    if (!thick) {
        if (l.mode != 0 && l.mode % 4 == 0 && l.elementLog2 <= 1) {
            tailWidth = bw / 4;
            tailHeight = bh >> (1 - l.elementLog2);
        } else {
            tailWidth = bw / 2;
            tailHeight = bh;
        }
    } else {
        switch (blockLog2 % 3) {
        case 0: tailWidth = bw; tailHeight = bh / 2; break;
        case 1: tailWidth = bw / 2; tailHeight = bh; break;
        default: tailWidth = bw; tailHeight = bh; break;
        }
    }
    const std::size_t effective = thick ? blockLog2 - (blockLog2 - 8) / 3 : blockLog2;
    std::size_t maxTail = 0;
    if (effective <= 8) maxTail = 0;
    else if (effective <= 11) maxTail = 1 + (std::size_t{1} << (effective - 9));
    else maxTail = effective - 4;
    std::size_t firstTail = levelCount;
    if (levelCount > 1 && maxTail > 0) {
        for (std::size_t i = 0; i < levelCount; ++i) {
            const auto [pw, ph] = padded(i);
            if (pw <= tailWidth && ph <= tailHeight && levelCount - i <= maxTail) {
                firstTail = i;
                break;
            }
        }
    }
    Chain chain;
    chain.levels.resize(levelCount);
    std::size_t offset = firstTail < levelCount ? l.blockBytes : 0;
    for (std::size_t i = firstTail; i-- > 0;) {
        const auto [w, h] = size(i);
        const auto [pw, ph] = padded(i);
        MipLevel& level = chain.levels[i];
        level.width = w;
        level.height = h;
        level.depth = depthOf(i);
        level.offset = offset;
        level.pitch = DivCeil(pw, bw) * bw;
        offset += l.Size(pw, ph);
    }
    const std::size_t e = l.elementLog2;
    constexpr std::size_t kThickW[] = {3, 2, 2, 2, 1};
    constexpr std::size_t kThickH[] = {2, 2, 2, 1, 1};
    constexpr std::size_t kThinW[] = {4, 4, 3, 3, 2};
    constexpr std::size_t kThinH[] = {4, 3, 3, 2, 2};
    const std::size_t w256 = std::size_t{1} << (thick ? kThickW[e] : kThinW[e]);
    const std::size_t h256 = std::size_t{1} << (thick ? kThickH[e] : kThinH[e]);
    for (std::size_t i = firstTail; i < levelCount; ++i) {
        const std::size_t m = maxTail - 1 - (i - firstTail);
        const std::size_t at = m > 6 ? (std::size_t{16} << m) : (m << 8);
        std::size_t mx = 0;
        std::size_t my = 0;
        for (std::size_t b = 0; b < 6; ++b) {
            mx |= ((at >> (9 + 2 * b)) & 1u) << b;
            my |= ((at >> (8 + 2 * b)) & 1u) << b;
        }
        const auto [w, h] = size(i);
        MipLevel& level = chain.levels[i];
        level.width = w;
        level.height = h;
        level.depth = depthOf(i);
        level.offset = 0;
        level.pitch = bw;
        level.originX = mx * w256;
        level.originY = my * h256;
        level.tail = at;
    }
    chain.layout = std::move(layout);
    chain.sliceBytes = offset;
    return chain;
}

std::size_t Chain::Layers() const {
    return DivCeil(levels.front().depth, layout->blockDepth);
}

std::size_t Chain::ElementAt(const MipLevel& level, std::size_t x, std::size_t y, std::size_t slice) const {
    return level.offset + layout->Offset(x + level.originX, y + level.originY, slice, level.pitch);
}

std::size_t Chain::ElementAtZ(const MipLevel& level, std::size_t x, std::size_t y, std::size_t z) const {
    return (z / layout->blockDepth) * sliceBytes + level.offset + layout->Offset(x + level.originX, y + level.originY, z, level.pitch);
}

std::vector<std::uint8_t> Chain::DetileLevel(std::span<const std::uint8_t> slice, std::size_t mip, std::size_t sliceIndex) const {
    if (mip >= levels.size()) return {};
    const MipLevel& level = levels[mip];
    const std::size_t element = std::size_t{1} << layout->elementLog2;
    std::vector<std::uint8_t> out(level.width * level.height * element);
    for (std::size_t y = 0; y < level.height; ++y) {
        for (std::size_t x = 0; x < level.width; ++x) {
            const std::size_t at = ElementAt(level, x, y, sliceIndex);
            if (at + element <= slice.size()) std::memcpy(out.data() + (y * level.width + x) * element, slice.data() + at, element);
        }
    }
    return out;
}

bool Chain::TileLevel(std::span<std::uint8_t> slice, std::size_t mip, std::size_t sliceIndex, std::span<const std::uint8_t> linear) const {
    if (mip >= levels.size()) return false;
    const MipLevel& level = levels[mip];
    const std::size_t element = std::size_t{1} << layout->elementLog2;
    if (linear.size() < level.width * level.height * element) return false;
    for (std::size_t y = 0; y < level.height; ++y) {
        for (std::size_t x = 0; x < level.width; ++x) {
            const std::size_t at = ElementAt(level, x, y, sliceIndex);
            if (at + element > slice.size()) return false;
            std::memcpy(slice.data() + at, linear.data() + (y * level.width + x) * element, element);
        }
    }
    return true;
}

std::vector<std::uint8_t> Chain::DetileVolume(std::span<const std::uint8_t> memory, std::size_t mip) const {
    if (mip >= levels.size()) return {};
    const MipLevel& level = levels[mip];
    const std::size_t element = std::size_t{1} << layout->elementLog2;
    std::vector<std::uint8_t> out(level.width * level.height * level.depth * element);
    for (std::size_t z = 0; z < level.depth; ++z) {
        for (std::size_t y = 0; y < level.height; ++y) {
            for (std::size_t x = 0; x < level.width; ++x) {
                const std::size_t at = ElementAtZ(level, x, y, z);
                if (at + element <= memory.size()) std::memcpy(out.data() + ((z * level.height + y) * level.width + x) * element, memory.data() + at, element);
            }
        }
    }
    return out;
}

bool Chain::TileVolume(std::span<std::uint8_t> memory, std::size_t mip, std::span<const std::uint8_t> linear) const {
    if (mip >= levels.size()) return false;
    const MipLevel& level = levels[mip];
    const std::size_t element = std::size_t{1} << layout->elementLog2;
    if (linear.size() < level.width * level.height * level.depth * element) return false;
    for (std::size_t z = 0; z < level.depth; ++z) {
        for (std::size_t y = 0; y < level.height; ++y) {
            for (std::size_t x = 0; x < level.width; ++x) {
                const std::size_t at = ElementAtZ(level, x, y, z);
                if (at + element > memory.size()) return false;
                std::memcpy(memory.data() + at, linear.data() + ((z * level.height + y) * level.width + x) * element, element);
            }
        }
    }
    return true;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> ImageFormatElement(std::uint32_t f) {
    if (f >= 1 && f <= 6) return std::make_pair(0u, 1u);
    if ((f >= 7 && f <= 19) || (f >= 133 && f <= 136)) return std::make_pair(1u, 1u);
    if ((f >= 20 && f <= 61) || f == 130 || f == 132) return std::make_pair(2u, 1u);
    if (f >= 62 && f <= 71) return std::make_pair(3u, 1u);
    if (f >= 75 && f <= 77) return std::make_pair(4u, 1u);
    if (f == 169 || f == 170 || f == 175 || f == 176) return std::make_pair(3u, 4u);
    if ((f >= 171 && f <= 174) || (f >= 177 && f <= 182)) return std::make_pair(4u, 4u);
    return std::nullopt;
}

std::optional<ImageSurface> ImageLayout(std::uint32_t imageFormat, std::uint32_t swizzleMode, std::uint64_t address, std::uint32_t width, std::uint32_t height, PipeConfig pipes) {
    const std::optional<std::pair<std::uint32_t, std::uint32_t>> element = ImageFormatElement(imageFormat);
    if (!element) return std::nullopt;
    const std::shared_ptr<const Layout> shared = Layout2d(swizzleMode, element->first, pipes);
    if (!shared) return std::nullopt;
    ImageSurface surface;
    surface.layout = *shared;
    surface.layout.baseXor = static_cast<std::size_t>(address) & (surface.layout.blockBytes - 1);
    surface.width = DivCeil(width, element->second);
    surface.height = DivCeil(height, element->second);
    return surface;
}

std::uint64_t Surface::Size() const {
    const std::size_t slices = depth > 0 ? chain.Layers() : array;
    return static_cast<std::uint64_t>(chain.sliceBytes) * slices;
}

std::uint64_t Surface::Alignment() const {
    return chain.layout->blockBytes;
}

std::pair<std::uint32_t, std::uint32_t> Surface::Slices(std::size_t mip) const {
    if (depth == 0) return {array, array};
    const std::uint32_t d = (std::max)(depth >> mip, 1u);
    if (mode == 0) return {d, depth};
    const std::size_t blockDepth = chain.layout->blockDepth;
    if (blockDepth == 1 && chain.levels.size() > 1) return {d, 1u};
    const std::size_t halved = DivCeil(depth, std::size_t{1} << mip);
    return {d, static_cast<std::uint32_t>(DivCeil(halved, blockDepth) * blockDepth)};
}

std::pair<std::size_t, std::size_t> Surface::Padded(std::size_t mip) const {
    const Layout& l = *chain.layout;
    if (mip < firstTail) {
        const std::size_t h = DivCeil(DivCeil(height, side), std::size_t{1} << mip);
        return {chain.levels[mip].pitch, DivCeil(h, l.blockHeight) * l.blockHeight};
    }
    const auto [mw, mh] = MicroBlock(l.elementLog2);
    const std::size_t k = mip - firstTail;
    return {(std::max)((l.blockWidth / 2) >> k, mw), (std::max)(l.blockHeight >> k, mh)};
}

std::uint64_t Surface::MipOffset(std::size_t mip) const {
    const MipLevel& level = chain.levels[mip];
    return level.tail ? *level.tail : level.offset;
}

std::uint64_t Surface::ElementOffset(std::size_t mip, std::size_t x, std::size_t y, std::size_t slice) const {
    const MipLevel& level = chain.levels[mip];
    if (chain.layout->blockDepth > 1) return chain.ElementAtZ(level, x, y, slice);
    return static_cast<std::uint64_t>(slice) * chain.sliceBytes + level.offset + chain.layout->Offset(x + level.originX, y + level.originY, slice, level.pitch);
}

void Surface::WritePlane(std::span<std::uint8_t, kResourceLayoutBytes> layout, std::size_t planeBase) const {
    const std::size_t mips = (std::min)(chain.levels.size(), std::size_t{kMaxMips});
    const std::size_t tailStart = (std::min)(firstTail, mips);
    Store64(layout, planeBase + 0x08, Size());
    Store64(layout, planeBase + 0x10, chain.sliceBytes);
    Store64(layout, planeBase + 0x20, Alignment());
    Store32(layout, planeBase + 0x28, bytesPerElement);
    Store32(layout, planeBase + 0x2C, static_cast<std::uint32_t>(tailStart));
    Store32(layout, planeBase + 0x30, static_cast<std::uint32_t>(mips - tailStart));
    for (std::size_t i = 0; i < mips; ++i) {
        const MipLevel& level = chain.levels[i];
        const std::size_t r = planeBase + kLayoutMipBase + i * kLayoutMipBytes;
        const bool tail = i >= firstTail;
        const auto [pw, ph] = Padded(i);
        const std::uint64_t bytes = static_cast<std::uint64_t>(pw) * ph * bytesPerElement;
        const auto [slices, paddedSlices] = Slices(i);
        Store64(layout, r + 0x00, depth > 0 ? bytes * paddedSlices : bytes);
        Store64(layout, r + 0x08, MipOffset(i));
        Store64(layout, r + 0x10, bytes);
        Store32(layout, r + 0x18, static_cast<std::uint32_t>(pw) * side);
        Store32(layout, r + 0x1C, static_cast<std::uint32_t>(pw) * bytesPerElement);
        Store32(layout, r + 0x20, tail ? 0x100u : static_cast<std::uint32_t>(Alignment()));
        Store32(layout, r + 0x24, static_cast<std::uint32_t>(pw));
        Store32(layout, r + 0x28, static_cast<std::uint32_t>(ph));
        Store32(layout, r + 0x2C, paddedSlices);
        Store32(layout, r + 0x30, static_cast<std::uint32_t>(level.width));
        Store32(layout, r + 0x34, static_cast<std::uint32_t>(level.height));
        Store32(layout, r + 0x38, (std::max)(width >> i, 1u));
        Store32(layout, r + 0x3C, (std::max)(height >> i, 1u));
        Store32(layout, r + 0x40, slices);
        Store32(layout, r + 0x44, 1);
        Store32(layout, r + 0x48, tail && mode % 4 != 0 ? mode % 4 : mode);
        if (!tail && mode >= 24 && mode <= 27) {
            Store32(layout, r + 0x50, 0x8000);
            Store32(layout, r + 0x58, 0x8000);
        }
        Store32(layout, r + 0x60, static_cast<std::uint32_t>(chain.layout->blockDepth));
        Store32(layout, r + 0x64, tail ? 1u : 0u);
    }
}

std::uint64_t Plane::Bytes() const {
    if (const Surface* surface = std::get_if<Surface>(&kind)) return surface->Size();
    if (const Metadata* meta = std::get_if<Metadata>(&kind)) return meta->bytes;
    return 0;
}

std::uint64_t Plane::Alignment() const {
    if (const Surface* surface = std::get_if<Surface>(&kind)) return surface->Alignment();
    if (const Metadata* meta = std::get_if<Metadata>(&kind)) return meta->alignment;
    return 0;
}

std::uint64_t ConsoleLayout::Size() const {
    if (empty) return 0;
    std::uint64_t end = 0;
    for (const Plane& plane : planes) end = (std::max)(end, plane.offset + plane.Bytes());
    return end;
}

std::uint64_t ConsoleLayout::Alignment() const {
    if (planes.empty()) return 0x100;
    std::uint64_t alignment = 0;
    for (const Plane& plane : planes) alignment = (std::max)(alignment, plane.Alignment());
    return alignment;
}

std::optional<PlaneView> ConsoleLayout::PlaneSurface(std::uint32_t plane) const {
    if (plane >= planes.size()) return std::nullopt;
    const Surface* surface = std::get_if<Surface>(&planes[plane].kind);
    if (surface == nullptr) return std::nullopt;
    return PlaneView{surface, planes[plane].offset};
}

const Surface* ConsoleLayout::Texels() const {
    const std::optional<PlaneView> view = PlaneSurface(static_cast<std::uint32_t>(main));
    return view ? view->surface : nullptr;
}

std::uint64_t ConsoleLayout::MipOffset(std::size_t mip) const {
    const std::optional<PlaneView> view = PlaneSurface(static_cast<std::uint32_t>(main));
    if (!view || mip >= view->surface->chain.levels.size()) return 0;
    return view->offset + view->surface->MipOffset(mip);
}

std::uint64_t ConsoleLayout::ElementOffset(std::size_t mip, std::size_t x, std::size_t y, std::size_t slice) const {
    const std::optional<PlaneView> view = PlaneSurface(static_cast<std::uint32_t>(main));
    if (!view || mip >= view->surface->chain.levels.size()) return 0;
    return view->offset + view->surface->ElementOffset(mip, x, y, slice);
}

void ConsoleLayout::Write(std::span<std::uint8_t, kResourceLayoutBytes> layout) const {
    std::fill(layout.begin(), layout.end(), std::uint8_t{0});
    Store64(layout, 0x00, Size());
    Store64(layout, 0x08, Alignment());
    Store32(layout, 0x10, mips);
    Store32(layout, 0x14, static_cast<std::uint32_t>(planes.size()));
    Store32(layout, kLayoutDimension, dimension);
    for (std::size_t k = 0; k < planes.size(); ++k) {
        const std::size_t base = kLayoutPlaneBase + k * kLayoutPlaneBytes;
        if (base + kLayoutPlaneBytes > kLayoutDimension) break;
        const Plane& plane = planes[k];
        Store32(layout, base, plane.usage);
        Store64(layout, base + 0x18, plane.offset);
        if (const Surface* surface = std::get_if<Surface>(&plane.kind)) {
            surface->WritePlane(layout, base);
        } else if (const Metadata* meta = std::get_if<Metadata>(&plane.kind)) {
            Store64(layout, base + 0x08, meta->bytes);
            Store64(layout, base + 0x20, meta->alignment);
            Store64(layout, base + kLayoutMipBase, meta->bytes);
            Store32(layout, base + kLayoutMipBase + 0x20, static_cast<std::uint32_t>(meta->alignment));
        }
    }
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> FormatElement(std::uint32_t f) {
    if (f >= 1 && f <= 4) return std::make_pair(4u, 1u);
    if (f >= 9 && f <= 22) return std::make_pair(3u, 1u);
    if ((f >= 23 && f <= 47) || f == 67 || (f >= 87 && f <= 93) || f == 116 || f == 117) return std::make_pair(2u, 1u);
    if ((f >= 48 && f <= 59) || f == 85 || f == 86 || f == 115) return std::make_pair(1u, 1u);
    if (f >= 60 && f <= 65) return std::make_pair(0u, 1u);
    if ((f >= 70 && f <= 72) || (f >= 79 && f <= 81)) return std::make_pair(3u, 4u);
    if ((f >= 73 && f <= 78) || (f >= 82 && f <= 84) || (f >= 94 && f <= 99)) return std::make_pair(4u, 4u);
    return std::nullopt;
}

std::uint32_t OptimalMode(std::uint32_t layout, std::uint32_t flags, std::uint64_t bitsPerPixel, std::uint64_t width, std::uint32_t height) {
    const bool renderTarget = (flags & 1u) != 0;
    const bool depth = (flags & 2u) != 0;
    const bool unordered = (flags & 4u) != 0;
    if (depth) return 24;
    if (layout == 2) return 10;
    if (renderTarget) return 27;
    if (!unordered) return 5;
    if (bitsPerPixel * width * height <= 0x80000) return 21;
    return 27;
}

std::optional<ConsoleLayout> DescribeConsoleLayout(const D3D12_RESOURCE_DESC& desc, PipeConfig pipes, Refusal& refusal) {
    const std::uint32_t dimension = static_cast<std::uint32_t>(desc.Dimension);
    const std::uint64_t width = desc.Width;
    const std::uint32_t height = desc.Height;
    const std::uint32_t array = desc.DepthOrArraySize;
    const std::uint32_t mips = desc.MipLevels;
    const std::uint32_t format = static_cast<std::uint32_t>(desc.Format);
    const std::uint32_t samples = desc.SampleDesc.Count;
    const std::uint32_t layoutValue = static_cast<std::uint32_t>(desc.Layout);
    const std::uint32_t flags = static_cast<std::uint32_t>(desc.Flags);
    if (dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D && samples <= 1) return Volume(width, height, array, mips, format, layoutValue, flags, pipes, refusal);
    if ((dimension != D3D12_RESOURCE_DIMENSION_TEXTURE1D && dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) || samples > 1) {
        refusal = {E_NOTIMPL, "buffers and multisampled textures have no console layout model"};
        return std::nullopt;
    }
    const bool line = dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    if (line && (height != 1 || (flags & 3u) != 0 || !(layoutValue == 0 || layoutValue == 1 || layoutValue == 0x100 || layoutValue == 0x11B))) {
        refusal = {E_INVALIDARG, "a 1D texture with a layout the console's library refuses"};
        return std::nullopt;
    }
    if (width == 0 || width > 16384 || height == 0 || height > 16384 || array == 0) {
        refusal = {E_INVALIDARG, "a texture outside 1..16384 or with no slices"};
        return std::nullopt;
    }
    const bool d16s8 = format == 118 || format == 120 || (format == 119 && (flags & 3u) == 2 && mips == 1);
    const bool planar = (format >= 19 && format <= 22) || (format >= 44 && format <= 47) || d16s8;
    std::uint32_t elementLog2 = 0;
    std::uint32_t side = 1;
    if (d16s8 || format == 119) {
        elementLog2 = 1;
    } else if (planar) {
        elementLog2 = 2;
    } else {
        const std::optional<std::pair<std::uint32_t, std::uint32_t>> element = FormatElement(format);
        if (!element) {
            refusal = {E_INVALIDARG, "format " + std::to_string(format) + " has no tiled element size"};
            return std::nullopt;
        }
        elementLog2 = element->first;
        side = element->second;
    }
    std::uint32_t mode = 0;
    if (layoutValue == 1 || (layoutValue == 0 && line)) {
        mode = 0;
    } else if (layoutValue >= 0x100 && layoutValue <= 0x11F) {
        mode = layoutValue & 0x1Fu;
    } else if (layoutValue == 0 || layoutValue == 2) {
        const std::uint64_t bits = (format >= 19 && format <= 22) ? 64 : (std::uint64_t{8} << elementLog2) / (side * side);
        mode = OptimalMode(layoutValue, flags, bits, width, height);
    } else {
        refusal = {E_INVALIDARG, "texture layout " + std::to_string(layoutValue) + " is not one the console's library takes"};
        return std::nullopt;
    }
    const std::uint32_t full = FullMips((std::max)(width, static_cast<std::uint64_t>(height)));
    const std::uint32_t levels = mips == 0 ? full : mips;
    if (levels > kMaxMips) {
        refusal = {E_INVALIDARG, std::to_string(levels) + " mips are more than the console's 15"};
        return std::nullopt;
    }
    const bool renderTarget = (flags & 1u) != 0;
    const bool depthStencil = (flags & 2u) != 0;
    if (planar && layoutValue == 2 && format != 119) {
        refusal = {E_INVALIDARG, "a stencil format with the 64KB_D layout"};
        return std::nullopt;
    }
    const bool depthBuffer = depthStencil && levels == 1 && (planar || !renderTarget);
    if (depthBuffer && mode != 24) {
        refusal = {E_INVALIDARG, "a depth buffer outside swizzle mode 24"};
        return std::nullopt;
    }
    const bool depthFormat = planar || format == 39 || format == 40 || format == 53 || format == 55;
    const bool blank = (depthBuffer && !depthFormat) || ((renderTarget || depthStencil) && side > 1 && levels == 1);
    const bool empty = blank && depthStencil;
    const bool stencilOnly = format == 120 && !depthBuffer;
    const std::uint32_t surfaceWidth = static_cast<std::uint32_t>(width);
    std::variant<Surface, Metadata, Blank> main = Blank{};
    if (!blank && !stencilOnly) {
        std::optional<Surface> surface = MakeSurface(mode, elementLog2, side, surfaceWidth, height, array, levels, pipes, refusal);
        if (!surface) return std::nullopt;
        main = std::move(*surface);
    }
    std::optional<Surface> stencil;
    if (planar) {
        stencil = MakeSurface(mode, 0, 1, surfaceWidth, height, array, levels, pipes, refusal);
        if (!stencil) return std::nullopt;
    }
    const auto meta = [&](std::uint64_t divisor) { return (DivCeil64(width, 1024) * 1024) * (DivCeil64(height, 1024) * 1024) / divisor * array; };
    const std::uint32_t mainUsage = depthStencil ? kUsageDepthBufferDepth : planar ? kUsageDepth : kUsageColor;
    const std::uint32_t stencilUsage = depthStencil ? kUsageDepthBufferStencil : planar ? kUsageStencil : 0;
    std::vector<Plane> planes;
    if (depthBuffer) planes.push_back(Plane{kUsageHtile, 0, Metadata{meta(16), 0x10000}});
    const std::size_t mainAt = planes.size();
    planes.push_back(Plane{depthStencil && !planar && !depthBuffer ? kUsageColor : mainUsage, 0, std::move(main)});
    if (stencil) planes.push_back(Plane{stencilUsage, 0, std::move(*stencil)});
    if (renderTarget && !depthBuffer && !planar && !empty && levels == 1 && layoutValue != 2 && !(layoutValue >= 0x110 && layoutValue <= 0x113)) {
        planes.push_back(Plane{kUsageCmask, 0, Metadata{meta(128), 0x2000}});
    }
    std::uint64_t offset = 0;
    for (Plane& plane : planes) {
        if (std::holds_alternative<Blank>(plane.kind)) {
            plane.offset = 0;
            continue;
        }
        const std::uint64_t align = plane.Alignment();
        plane.offset = DivCeil64(offset, align) * align;
        offset = (std::max)(offset, plane.offset + plane.Bytes());
    }
    ConsoleLayout result;
    result.planes = std::move(planes);
    result.main = mainAt;
    result.mips = levels;
    result.empty = empty;
    result.dimension = dimension;
    return result;
}

std::uint32_t SuggestSwizzleLayout(const D3D12_RESOURCE_DESC& desc, PipeConfig pipes, Refusal& fallback) {
    constexpr std::uint32_t kExplicitLayout = 0x100;
    D3D12_RESOURCE_DESC chosen = desc;
    chosen.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (chosen.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D) return kExplicitLayout;
    Refusal refusal;
    const std::optional<ConsoleLayout> layout = DescribeConsoleLayout(chosen, pipes, refusal);
    const Surface* texels = layout ? layout->Texels() : nullptr;
    if (texels != nullptr) return kExplicitLayout | texels->mode;
    fallback = layout ? Refusal{E_NOTIMPL, "the console's layout library leaves this texture's texels out"} : refusal;
    const std::optional<std::pair<std::uint32_t, std::uint32_t>> element = FormatElement(static_cast<std::uint32_t>(chosen.Format));
    const std::uint64_t bits = element ? std::uint64_t{8} << element->first : 32;
    return kExplicitLayout | OptimalMode(0, static_cast<std::uint32_t>(chosen.Flags), bits, chosen.Width, chosen.Height);
}

HRESULT CopyIntoSubresource(const ConsoleLayout& layout, std::uint8_t* resource, std::uint32_t plane, std::uint32_t subresource, const std::uint8_t* source, std::uint32_t rowPitch,
                            std::uint32_t slicePitch, std::string& error) {
    const std::optional<SubresourceView> view = resource != nullptr && source != nullptr ? FindSubresource(layout, plane, subresource) : std::nullopt;
    if (!view) {
        error = "CopyIntoSubresource with a null pointer, a plane without texels or a subresource past the texture";
        return E_INVALIDARG;
    }
    const Surface& surface = *view->surface;
    const MipLevel& level = surface.chain.levels[view->mip];
    const std::size_t row = level.width * surface.bytesPerElement;
    if (surface.depth > 0) {
        std::vector<std::uint8_t> linear(row * level.height * level.depth);
        for (std::size_t n = 0; n < level.height * level.depth; ++n) {
            const std::size_t from = n / level.height * slicePitch + n % level.height * rowPitch;
            std::memcpy(linear.data() + n * row, source + from, row);
        }
        if (!surface.chain.TileVolume(std::span<std::uint8_t>(resource + view->offset, static_cast<std::size_t>(surface.Size())), view->mip, linear)) {
            error = "a texel of the volume level lies outside the surface";
            return E_FAIL;
        }
        return S_OK;
    }
    std::vector<std::uint8_t> linear(row * level.height);
    for (std::size_t y = 0; y < level.height; ++y) std::memcpy(linear.data() + y * row, source + y * rowPitch, row);
    std::uint8_t* slice = resource + view->offset + view->slice * surface.chain.sliceBytes;
    if (!surface.chain.TileLevel(std::span<std::uint8_t>(slice, surface.chain.sliceBytes), view->mip, view->slice, linear)) {
        error = "a texel of the level lies outside its slice";
        return E_FAIL;
    }
    return S_OK;
}

HRESULT CopyFromSubresource(const ConsoleLayout& layout, std::uint8_t* destination, std::uint32_t plane, std::uint32_t subresource, const std::uint8_t* resource, std::uint32_t rowPitch,
                            std::uint32_t slicePitch, std::string& error) {
    const std::optional<SubresourceView> view = destination != nullptr && resource != nullptr ? FindSubresource(layout, plane, subresource) : std::nullopt;
    if (!view) {
        error = "CopyFromSubresource with a null pointer, a plane without texels or a subresource past the texture";
        return E_INVALIDARG;
    }
    const Surface& surface = *view->surface;
    const MipLevel& level = surface.chain.levels[view->mip];
    const std::size_t row = level.width * surface.bytesPerElement;
    if (surface.depth > 0) {
        const std::vector<std::uint8_t> linear =
            surface.chain.DetileVolume(std::span<const std::uint8_t>(resource + view->offset, static_cast<std::size_t>(surface.Size())), view->mip);
        for (std::size_t n = 0; n < level.height * level.depth; ++n) {
            const std::size_t to = n / level.height * slicePitch + n % level.height * rowPitch;
            std::memcpy(destination + to, linear.data() + n * row, row);
        }
        return S_OK;
    }
    const std::uint8_t* slice = resource + view->offset + view->slice * surface.chain.sliceBytes;
    const std::vector<std::uint8_t> linear = surface.chain.DetileLevel(std::span<const std::uint8_t>(slice, surface.chain.sliceBytes), view->mip, view->slice);
    for (std::size_t y = 0; y < level.height; ++y) std::memcpy(destination + y * rowPitch, linear.data() + y * row, row);
    return S_OK;
}

HRESULT ResourceTiling(const ConsoleLayout& layout, UINT* totalTiles, D3D12_PACKED_MIP_INFO* packed, D3D12_TILE_SHAPE* shape, UINT* count, UINT first, D3D12_SUBRESOURCE_TILING* tilings,
                       std::string& error) {
    const Surface* surface = layout.Texels();
    if (surface == nullptr) {
        error = "resource tiling of a texture whose texels the console's library leaves out";
        return E_FAIL;
    }
    if (surface->depth > 0) {
        error = "resource tiling of a 3D texture has no model";
        return E_NOTIMPL;
    }
    const Layout& l = *surface->chain.layout;
    if (l.blockBytes != kTiledResourceTileBytes) {
        error = "resource tiling of swizzle mode " + std::to_string(surface->mode) + ", whose blocks are not 64 KiB tiles";
        return E_FAIL;
    }
    const std::size_t mips = surface->chain.levels.size();
    const std::size_t standard = (std::min)(surface->firstTail, mips);
    if (totalTiles != nullptr) *totalTiles = static_cast<UINT>(DivCeil64(surface->Size(), kTiledResourceTileBytes));
    if (packed != nullptr) {
        const std::uint32_t packedMips = static_cast<std::uint32_t>(mips - standard);
        const std::uint32_t words[3] = {static_cast<std::uint32_t>(standard) | packedMips << 8, packedMips > 0 ? 1u : 0u, 0u};
        static_assert(sizeof(D3D12_PACKED_MIP_INFO) == sizeof(words));
        std::memcpy(packed, words, sizeof(words));
    }
    if (shape != nullptr) {
        shape->WidthInTexels = static_cast<UINT>(l.blockWidth) * surface->side;
        shape->HeightInTexels = static_cast<UINT>(l.blockHeight) * surface->side;
        shape->DepthInTexels = static_cast<UINT>(l.blockDepth);
    }
    if (count != nullptr) {
        const std::size_t asked = *count;
        const std::size_t subresources = mips * surface->array;
        UINT written = 0;
        for (std::size_t k = 0; k < asked; ++k) {
            const std::size_t sub = first + k;
            if (sub >= subresources) break;
            const std::size_t mip = sub % mips;
            const std::size_t slice = sub / mips;
            if (mip >= standard) continue;
            ++written;
            if (tilings != nullptr) {
                const auto [pw, ph] = surface->Padded(mip);
                const std::uint64_t start = (static_cast<std::uint64_t>(slice) * surface->chain.sliceBytes + surface->MipOffset(mip)) / kTiledResourceTileBytes;
                tilings[k].WidthInTiles = static_cast<UINT>(DivCeil(pw, l.blockWidth));
                tilings[k].HeightInTiles = static_cast<UINT16>(DivCeil(ph, l.blockHeight));
                tilings[k].DepthInTiles = 1;
                tilings[k].StartTileIndexInOverallResource = static_cast<UINT>(start);
            }
        }
        *count = written;
    }
    return S_OK;
}

std::optional<ConsoleTexture> DescribeConsoleTexture(const D3D12_RESOURCE_DESC& desc, PipeConfig pipes, std::string& error) {
    Refusal refusal;
    const std::optional<ConsoleLayout> layout = DescribeConsoleLayout(desc, pipes, refusal);
    if (!layout) {
        error = refusal.reason;
        return std::nullopt;
    }
    const std::optional<PlaneView> view = layout->PlaneSurface(static_cast<std::uint32_t>(layout->main));
    if (!view) {
        error = "the console's layout library leaves this texture's texels out";
        return std::nullopt;
    }
    const Surface& surface = *view->surface;
    ConsoleTexture texture;
    texture.chain = surface.chain;
    texture.mode = surface.mode;
    texture.bytesPerElement = surface.bytesPerElement;
    texture.side = surface.side;
    texture.array = surface.array;
    texture.depth = surface.depth;
    texture.mips = layout->mips;
    texture.texelOffset = view->offset;
    texture.texelBytes = surface.Size();
    texture.size = layout->Size();
    texture.alignment = layout->Alignment();
    return texture;
}

namespace {

template <std::size_t Element>
void DetileRow(const std::uint8_t* texels, std::size_t firstByte, std::size_t texelBytes, std::size_t rowBase, std::size_t rowBits, const std::size_t* columnBase,
               const std::uint32_t* columnBits, std::size_t width, std::uint8_t* out) {
    for (std::size_t x = 0; x < width; ++x) {
        const std::size_t at = rowBase + columnBase[x] + (rowBits ^ columnBits[x]);
        if (at >= firstByte && at - firstByte + Element <= texelBytes) std::memcpy(out + x * Element, texels + (at - firstByte), Element);
        else std::memset(out + x * Element, 0, Element);
    }
}

}

std::optional<std::pair<std::size_t, std::size_t>> SubresourceBytes(const ConsoleTexture& texture, std::uint32_t mip, std::uint32_t slice) {
    const Chain& chain = texture.chain;
    if (mip >= chain.levels.size()) return std::nullopt;
    const MipLevel& level = chain.levels[mip];
    const Layout& layout = *chain.layout;
    const bool volume = texture.depth > 0;
    const std::size_t depth = volume ? level.depth : 1;
    if (level.width == 0 || level.height == 0 || depth == 0 || (!volume && slice >= texture.array)) return std::nullopt;
    std::size_t bits = layout.baseXor;
    for (const std::vector<std::uint32_t>* table : {&layout.ox, &layout.oy, &layout.oz, &layout.hx, &layout.hy, &layout.hz}) {
        for (const std::uint32_t value : *table) bits |= value;
    }
    const std::size_t rowBlockBytes = DivCeil(level.pitch, layout.blockWidth) * layout.blockBytes;
    const std::size_t firstSlice = volume ? 0 : static_cast<std::size_t>(slice) * chain.sliceBytes;
    const std::size_t lastSlice = volume ? ((depth - 1) / layout.blockDepth) * chain.sliceBytes : firstSlice;
    const std::size_t first = firstSlice + level.offset + (level.originY / layout.blockHeight) * rowBlockBytes + (level.originX / layout.blockWidth) * layout.blockBytes;
    const std::size_t last = lastSlice + level.offset + ((level.originY + level.height - 1) / layout.blockHeight) * rowBlockBytes +
                             ((level.originX + level.width - 1) / layout.blockWidth) * layout.blockBytes + bits;
    return std::make_pair(first, last + (std::size_t{1} << layout.elementLog2));
}

bool DetileSubresource(const ConsoleTexture& texture, const std::uint8_t* texels, std::size_t texelBytes, std::uint32_t mip, std::uint32_t slice, std::uint8_t* destination,
                       std::size_t rowPitch, std::size_t rows, std::size_t rowBytes, std::string& error, std::size_t firstByte) {
    const Chain& chain = texture.chain;
    if (mip >= chain.levels.size()) {
        error = "mip " + std::to_string(mip) + " is past the console texture's " + std::to_string(chain.levels.size());
        return false;
    }
    const MipLevel& level = chain.levels[mip];
    const Layout& layout = *chain.layout;
    const std::size_t element = std::size_t{1} << layout.elementLog2;
    if (level.width * element != rowBytes || level.height != rows) {
        error = "mip " + std::to_string(mip) + " is " + std::to_string(level.width) + "x" + std::to_string(level.height) + " elements in the console layout but " +
                std::to_string(rowBytes) + " bytes x " + std::to_string(rows) + " rows in the copy footprint";
        return false;
    }
    const bool volume = texture.depth > 0;
    const std::size_t depth = volume ? level.depth : 1;
    if (!volume && slice >= texture.array) {
        error = "slice " + std::to_string(slice) + " is past the console texture's " + std::to_string(texture.array);
        return false;
    }
    const std::size_t pitchBlocks = DivCeil(level.pitch, layout.blockWidth);
    static thread_local std::vector<std::size_t> columnBase;
    static thread_local std::vector<std::uint32_t> columnBits;
    columnBase.resize(level.width);
    columnBits.resize(level.width);
    for (std::size_t x = 0; x < level.width; ++x) {
        const std::size_t tx = x + level.originX;
        const std::size_t block = tx / layout.blockWidth;
        columnBase[x] = block * layout.blockBytes;
        columnBits[x] = layout.ox[tx % layout.blockWidth] ^ layout.hx[block % layout.hx.size()];
    }
    for (std::size_t z = 0; z < depth; ++z) {
        const std::size_t zPattern = volume ? z : slice;
        const std::size_t sliceBase = volume ? (z / layout.blockDepth) * chain.sliceBytes : static_cast<std::size_t>(slice) * chain.sliceBytes;
        const std::uint32_t zBits = layout.oz[zPattern % layout.blockDepth] ^ layout.hz[(zPattern / layout.blockDepth) % layout.hz.size()];
        std::uint8_t* plane = destination + z * rows * rowPitch;
        for (std::size_t y = 0; y < level.height; ++y) {
            const std::size_t ty = y + level.originY;
            const std::size_t rowBase = sliceBase + level.offset + (ty / layout.blockHeight) * pitchBlocks * layout.blockBytes;
            const std::uint32_t yBits = zBits ^ layout.oy[ty % layout.blockHeight] ^ layout.hy[(ty / layout.blockHeight) % layout.hy.size()];
            const std::size_t rowBits = static_cast<std::size_t>(yBits) ^ layout.baseXor;
            std::uint8_t* out = plane + y * rowPitch;
            switch (element) {
            case 1:
                DetileRow<1>(texels, firstByte, texelBytes, rowBase, rowBits, columnBase.data(), columnBits.data(), level.width, out);
                break;
            case 2:
                DetileRow<2>(texels, firstByte, texelBytes, rowBase, rowBits, columnBase.data(), columnBits.data(), level.width, out);
                break;
            case 4:
                DetileRow<4>(texels, firstByte, texelBytes, rowBase, rowBits, columnBase.data(), columnBits.data(), level.width, out);
                break;
            case 8:
                DetileRow<8>(texels, firstByte, texelBytes, rowBase, rowBits, columnBase.data(), columnBits.data(), level.width, out);
                break;
            case 16:
                DetileRow<16>(texels, firstByte, texelBytes, rowBase, rowBits, columnBase.data(), columnBits.data(), level.width, out);
                break;
            default:
                for (std::size_t x = 0; x < level.width; ++x) {
                    const std::size_t at = rowBase + columnBase[x] + (rowBits ^ columnBits[x]);
                    if (at >= firstByte && at - firstByte + element <= texelBytes) std::memcpy(out + x * element, texels + (at - firstByte), element);
                    else std::memset(out + x * element, 0, element);
                }
                break;
            }
        }
    }
    return true;
}

}
