#include "Gfx10Tiling.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace abx::d3d12x::gfx10;

int g_failures = 0;

void Expect(bool condition, const std::string& what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

constexpr PipeConfig kNoPipes{0, 0};
constexpr PipeConfig kPipeConfigs[] = {{0, 0}, {1, 0}, {2, 0}, {3, 1}, {2, 2}, {3, 2}, {4, 2}, {3, 3}, {4, 3}, {5, 3}};

std::vector<std::uint8_t> Noise(std::size_t bytes, std::uint32_t& state) {
    std::vector<std::uint8_t> out(bytes);
    for (std::uint8_t& value : out) {
        state = state * 1103515245u + 12345u;
        value = static_cast<std::uint8_t>((state >> 16) | 1u);
    }
    return out;
}

void PatternsArePermutations() {
    const std::uint32_t modes[] = {1, 2, 5, 6, 9, 10, 17, 18, 21, 22, 24, 25, 26, 27};
    for (const PipeConfig pipes : kPipeConfigs) {
        for (const std::uint32_t mode : modes) {
            for (std::uint32_t e = 0; e <= 4; ++e) {
                const std::shared_ptr<const Layout> layout = Layout2d(mode, e, pipes);
                const std::string name = "mode " + std::to_string(mode) + " e " + std::to_string(e) + " pipes " + std::to_string(pipes.pipesLog2) + "/" + std::to_string(pipes.packersLog2);
                if (!layout) {
                    Expect(false, name + ": no layout");
                    continue;
                }
                Expect((layout->blockWidth * layout->blockHeight << e) == layout->blockBytes, name + ": block size");
                std::vector<bool> seen(layout->blockBytes >> e);
                bool ok = true;
                for (std::size_t y = 0; y < layout->blockHeight && ok; ++y) {
                    for (std::size_t x = 0; x < layout->blockWidth && ok; ++x) {
                        const std::size_t offset = layout->Offset(x, y, 0, layout->blockWidth);
                        if ((offset & ((std::size_t{1} << e) - 1)) != 0 || (offset >> e) >= seen.size() || seen[offset >> e]) ok = false;
                        else seen[offset >> e] = true;
                    }
                }
                Expect(ok, name + ": not a permutation of its block");
            }
        }
    }
}

void ThickPatternsArePermutations() {
    for (const PipeConfig pipes : kPipeConfigs) {
        for (const std::uint32_t mode : {5u, 9u, 17u, 21u, 25u, 26u}) {
            for (std::uint32_t e = 0; e <= 4; ++e) {
                const std::shared_ptr<const Layout> layout = Layout3d(mode, e, pipes);
                const std::string name =
                    "thick mode " + std::to_string(mode) + " e " + std::to_string(e) + " pipes " + std::to_string(pipes.pipesLog2) + "/" + std::to_string(pipes.packersLog2);
                if (!layout) {
                    Expect(false, name + ": no layout");
                    continue;
                }
                Expect(layout->blockDepth > 1, name + ": thick");
                Expect((layout->blockWidth * layout->blockHeight * layout->blockDepth << e) == layout->blockBytes, name + ": block size");
                std::vector<bool> seen(layout->blockBytes >> e);
                bool ok = true;
                for (std::size_t z = 0; z < layout->blockDepth && ok; ++z) {
                    for (std::size_t y = 0; y < layout->blockHeight && ok; ++y) {
                        for (std::size_t x = 0; x < layout->blockWidth && ok; ++x) {
                            const std::size_t offset = layout->Offset(x, y, z, layout->blockWidth);
                            if ((offset & ((std::size_t{1} << e) - 1)) != 0 || (offset >> e) >= seen.size() || seen[offset >> e]) ok = false;
                            else seen[offset >> e] = true;
                        }
                    }
                }
                Expect(ok, name + ": not a permutation of its block");
            }
        }
    }
    Expect(Layout3d(24, 2, kNoPipes) && Layout3d(24, 2, kNoPipes)->blockDepth == 1, "64KB_Z_X is thin for 3D without pipes");
    Expect(!Layout3d(10, 2, kNoPipes) && !Layout3d(1, 2, kNoPipes), "64KB_D and 256B_S are not 3D modes without pipes");
    Expect(Layout3d(24, 2, kSeriesXPipes) && Layout3d(24, 2, kSeriesXPipes)->blockDepth == 1, "64KB_Z_X is thin for 3D");
    Expect(!Layout3d(10, 2, kSeriesXPipes) && !Layout3d(1, 2, kSeriesXPipes), "64KB_D and 256B_S are not 3D modes");
}

void RetailChainSizes() {
    struct Case {
        std::uint32_t mode, e, side, width, height, mips;
        std::size_t bytes;
    };
    const Case cases[] = {
        {5, 4, 4, 128, 1024, 5, 192512}, {5, 2, 1, 128, 128, 7, 90112}, {5, 4, 4, 512, 64, 9, 65536},  {5, 3, 4, 64, 512, 9, 61440},
        {10, 4, 4, 2048, 2048, 1, 4194304}, {10, 4, 4, 512, 512, 8, 393216}, {5, 4, 4, 16, 16, 5, 4096},
    };
    for (const Case& c : cases) {
        const Chain chain = Chain::Make(Layout2d(c.mode, c.e, kNoPipes), c.width, c.height, 1, c.side, c.mips);
        Expect(chain.sliceBytes == c.bytes, std::to_string(c.width) + "x" + std::to_string(c.height) + " mode " + std::to_string(c.mode) + ": " + std::to_string(chain.sliceBytes) +
                                                " bytes, retail " + std::to_string(c.bytes));
    }
    struct Thick {
        std::uint32_t e, width, height, depth;
        std::size_t bytes;
    };
    for (const Thick& t : {Thick{1, 256, 32, 8, 262144}, Thick{3, 32, 64, 32, 524288}, Thick{3, 32, 32, 32, 262144}}) {
        const Chain chain = Chain::Make(Layout3d(5, t.e, kNoPipes), t.width, t.height, t.depth, 1, 1);
        Expect(chain.sliceBytes * chain.Layers() == t.bytes, "volume " + std::to_string(t.width) + "x" + std::to_string(t.height) + "x" + std::to_string(t.depth));
    }
    const std::shared_ptr<const Layout> thick = Layout3d(5, 1, kNoPipes);
    Expect(thick->blockWidth == 8 && thick->blockHeight == 16 && thick->blockDepth == 16, "4KB_S3 2-byte block is 8x16x16");
}

void LevelsDoNotOverlap() {
    struct Case {
        std::uint32_t mode, e, width, height, mips;
    };
    for (const Case& c : {Case{5, 2, 128, 96, 8}, Case{10, 4, 64, 64, 7}, Case{10, 0, 512, 128, 10}, Case{1, 2, 40, 24, 6}, Case{27, 2, 300, 200, 9}}) {
        const Chain chain = Chain::Make(Layout2d(c.mode, c.e, kSeriesXPipes), c.width, c.height, 1, 1, c.mips);
        std::vector<bool> seen(chain.sliceBytes >> c.e);
        bool ok = true;
        for (const MipLevel& level : chain.levels) {
            for (std::size_t y = 0; y < level.height && ok; ++y) {
                for (std::size_t x = 0; x < level.width && ok; ++x) {
                    const std::size_t at = (level.offset + chain.layout->Offset(x + level.originX, y + level.originY, 0, level.pitch)) >> c.e;
                    if (at >= seen.size() || seen[at]) ok = false;
                    else seen[at] = true;
                }
            }
        }
        Expect(ok, "mode " + std::to_string(c.mode) + " " + std::to_string(c.width) + "x" + std::to_string(c.height) + ": levels overlap or leave the slice");
    }
}

void TiledLevelsRoundTrip() {
    struct Case {
        std::uint32_t mode, e, width, height, mips;
    };
    for (const Case& c : {Case{5, 2, 128, 96, 8}, Case{10, 4, 64, 64, 7}, Case{10, 0, 512, 128, 10}, Case{1, 2, 40, 24, 6}}) {
        const Chain chain = Chain::Make(Layout2d(c.mode, c.e, kNoPipes), c.width, c.height, 1, 1, c.mips);
        const std::string name = "mode " + std::to_string(c.mode) + " e" + std::to_string(c.e);
        std::vector<std::uint8_t> slice(chain.sliceBytes);
        std::vector<std::vector<std::uint8_t>> want;
        std::uint32_t state = 0;
        bool tiled = true;
        for (std::size_t i = 0; i < chain.levels.size(); ++i) {
            std::vector<std::uint8_t> linear = Noise((chain.levels[i].width * chain.levels[i].height) << c.e, state);
            tiled = chain.TileLevel(slice, i, 0, linear) && tiled;
            want.push_back(std::move(linear));
        }
        Expect(tiled, name + ": a level does not fit its slice");
        for (std::size_t i = 0; i < want.size(); ++i) Expect(chain.DetileLevel(slice, i, 0) == want[i], name + ": level " + std::to_string(i) + " overwritten");
    }
}

void VolumeLevelsRoundTrip() {
    struct Case {
        std::uint32_t mode, e, width, height, depth, mips;
    };
    for (const Case& c : {Case{5, 2, 64, 32, 16, 7}, Case{9, 0, 128, 128, 64, 8}, Case{9, 4, 32, 32, 32, 6}, Case{24, 2, 64, 64, 4, 5}, Case{0, 1, 40, 8, 3, 4}}) {
        const std::string name = "volume mode " + std::to_string(c.mode) + " e" + std::to_string(c.e);
        const std::shared_ptr<const Layout> layout = Layout3d(c.mode, c.e, kNoPipes);
        if (!layout) {
            Expect(false, name + ": no layout");
            continue;
        }
        const Chain chain = Chain::Make(layout, c.width, c.height, c.depth, 1, c.mips);
        std::vector<std::uint8_t> memory(chain.sliceBytes * chain.Layers());
        std::vector<std::vector<std::uint8_t>> want;
        std::uint32_t state = 7;
        bool tiled = true;
        for (std::size_t i = 0; i < chain.levels.size(); ++i) {
            const MipLevel& level = chain.levels[i];
            std::vector<std::uint8_t> linear = Noise((level.width * level.height * level.depth) << c.e, state);
            tiled = chain.TileVolume(memory, i, linear) && tiled;
            want.push_back(std::move(linear));
        }
        Expect(tiled, name + ": a level does not fit the texture");
        for (std::size_t i = 0; i < want.size(); ++i) Expect(chain.DetileVolume(memory, i) == want[i], name + ": level " + std::to_string(i) + " overwritten");
    }
}

void ImageDescriptorLayout() {
    const std::optional<ImageSurface> image = ImageLayout(181, 10, 0x123450000ull | 0x300, 256, 130, kNoPipes);
    Expect(image && image->width == 64 && image->height == 33 && image->layout.blockWidth == 64 && image->layout.blockHeight == 64 && image->layout.baseXor == 0x300,
           "BC7 T# 256x130 in 64KB_D: 64x33 elements in 64x64 blocks with base swizzle 0x300");
    Expect(image && image->layout.Size(image->width, image->height) == 65536, "33 block rows fit one 64x64 tile");
    Expect(ImageFormatElement(56) == std::make_pair(2u, 1u), "image format 56 is 4 bytes");
    Expect(!ImageFormatElement(72), "image format 72 has no tiled element");
}

void TileThenDetileWithPartialBlocks() {
    const std::shared_ptr<const Layout> layout = Layout2d(25, 2, PipeConfig{4, 2});
    const std::size_t width = 200;
    const std::size_t height = 130;
    std::vector<std::uint8_t> linear(width * height * 4);
    for (std::size_t i = 0; i < linear.size(); ++i) linear[i] = static_cast<std::uint8_t>(i * 7 + i / 977);
    std::vector<std::uint8_t> tiled;
    Expect(layout && layout->Tile(linear, width, height, tiled), "64KB_S_X 200x130 tiles");
    Expect(tiled.size() == 2 * 2 * 65536, "200x130 4-byte elements take 2x2 64 KiB blocks");
    Expect(layout && layout->Detile(tiled, width, height) == linear, "tile then detile gives the rows back");
}

void LinearAndRowMajor() {
    const std::shared_ptr<const Layout> linear = Layout2d(0, 2, kNoPipes);
    Expect(linear->blockWidth == 64 && linear->blockHeight == 1 && linear->blockBytes == 256, "linear block");
    Expect(linear->Offset(3, 2, 0, 100) == (2 * 128 + 3) * 4, "linear rows padded to 256 bytes");
    const Chain chain = Chain::Make(linear, 100, 30, 1, 1, 3);
    Expect(chain.levels[2].height == 7 && chain.levels[2].offset == 0 && chain.levels[1].offset == 8 * 256 && chain.levels[0].offset == 8 * 256 + 15 * 256, "linear level offsets");
    Expect(chain.sliceBytes == 8 * 256 + 15 * 256 + 30 * 512, "linear slice bytes");
    const std::shared_ptr<const Layout> s256 = Layout2d(1, 0, kNoPipes);
    Expect(s256->blockWidth == 16 && s256->blockHeight == 16, "256B_S byte block");
    Expect(s256->Offset(5, 3, 0, 16) == 3 * 16 + 5, "256B_S bytes are row-major");
    Expect(s256->Offset(1, 16, 0, 32) == 2 * 256 + 1, "256B_S second block row");
}

void RetailSwizzleBits() {
    struct Case {
        std::uint32_t mode, e;
        const char* want;
    };
    const Case cases[] = {{10, 4, "xyxyyxyxyxyx"}, {5, 4, "yyxxyxyx"},       {10, 3, "xyxxyyxyxyxyx"},   {5, 3, "xyyxxyxyx"},   {10, 2, "xxyyxyyxyxyxyx"},
                          {5, 2, "xxyyyxyxyx"},     {10, 1, "xxxyyyxyxyxyxyx"}, {5, 1, "xxxyyyxyxyx"}, {10, 0, "xxxyyyxyyxyxyxyx"}, {5, 0, "xxxxyyyyyxyx"}};
    for (const Case& c : cases) {
        const std::shared_ptr<const Layout> layout = Layout2d(c.mode, c.e, kNoPipes);
        std::string got;
        for (std::size_t i = 0; c.want[i] != 0; ++i) {
            const std::uint32_t bit = 1u << (i + c.e);
            bool inX = false, inY = false;
            for (const std::uint32_t o : layout->ox) inX = inX || o == bit;
            for (const std::uint32_t o : layout->oy) inY = inY || o == bit;
            got.push_back(inX ? 'x' : inY ? 'y' : '?');
        }
        Expect(got == c.want, "mode " + std::to_string(c.mode) + " e " + std::to_string(c.e) + ": " + got + " vs retail " + c.want);
    }
}

void ConsoleDescriptions() {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 128;
    desc.Height = 1024;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 5;
    desc.Format = DXGI_FORMAT_BC7_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x105);
    std::string error;
    std::optional<ConsoleTexture> texture = DescribeConsoleTexture(desc, kNoPipes, error);
    Expect(texture && texture->size == 192512 && texture->texelOffset == 0 && texture->alignment == 4096, "BC7 128x1024 in 4KB_S: " + error);
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture = DescribeConsoleTexture(desc, kSeriesXPipes, error);
    Expect(texture && texture->mode == 5, "layout 0 of a sampled texture is the library's 4KB_S");
    desc.Format = static_cast<DXGI_FORMAT>(5);
    Expect(!DescribeConsoleTexture(desc, kSeriesXPipes, error), "a 96-bit format is refused");
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Width = 64;
    desc.Height = 64;
    desc.MipLevels = 1;
    desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x100);
    texture = DescribeConsoleTexture(desc, kSeriesXPipes, error);
    std::vector<std::uint8_t> tiled(texture ? texture->size : 0);
    for (std::size_t y = 0; y < 64 && texture; ++y) {
        for (std::size_t x = 0; x < 64; ++x) {
            const std::uint32_t value = static_cast<std::uint32_t>(y * 64 + x);
            std::memcpy(tiled.data() + texture->chain.layout->Offset(x, y, 0, texture->chain.levels[0].pitch), &value, 4);
        }
    }
    std::vector<std::uint8_t> linear(64 * 256);
    const bool detiled = texture && DetileSubresource(*texture, tiled.data(), tiled.size(), 0, 0, linear.data(), 256, 64, 256, error);
    bool ordered = detiled;
    for (std::uint32_t index = 0; index < 64 * 64 && ordered; ++index) {
        std::uint32_t value = 0;
        std::memcpy(&value, linear.data() + index * 4, 4);
        ordered = value == index;
    }
    Expect(ordered, "a linear texture detiles to its rows: " + error);
}

void DetileMatchesElementOffsets() {
    struct Case {
        DXGI_FORMAT format;
        UINT64 width;
        UINT height;
        UINT16 array;
        UINT16 mips;
        UINT layout;
    };
    const Case cases[] = {
        {DXGI_FORMAT_R8G8B8A8_UNORM, 300, 200, 1, 9, 0x100}, {DXGI_FORMAT_R8G8B8A8_UNORM, 512, 512, 2, 10, 0}, {DXGI_FORMAT_BC7_UNORM, 1024, 512, 1, 11, 0},
        {DXGI_FORMAT_R8_UNORM, 640, 360, 1, 1, 0},           {DXGI_FORMAT_BC1_UNORM, 256, 256, 3, 9, 0x105},   {DXGI_FORMAT_R16G16B16A16_FLOAT, 320, 180, 1, 1, 0},
    };
    for (const Case& item : cases) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = item.width;
        desc.Height = item.height;
        desc.DepthOrArraySize = item.array;
        desc.MipLevels = item.mips;
        desc.Format = item.format;
        desc.SampleDesc.Count = 1;
        desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(item.layout);
        std::string error;
        const std::optional<ConsoleTexture> texture = DescribeConsoleTexture(desc, kSeriesXPipes, error);
        const std::string name = "format " + std::to_string(static_cast<int>(item.format)) + " " + std::to_string(item.width) + "x" + std::to_string(item.height) + " layout " +
                                 std::to_string(item.layout);
        Expect(texture.has_value(), name + " is described: " + error);
        if (!texture) continue;
        const std::size_t bytes = static_cast<std::size_t>((std::min)(texture->texelBytes, texture->size - texture->texelOffset));
        std::vector<std::uint8_t> tiled(bytes);
        std::uint32_t seed = 0x1234567u + static_cast<std::uint32_t>(item.width);
        for (std::uint8_t& byte : tiled) {
            seed = seed * 1664525u + 1013904223u;
            byte = static_cast<std::uint8_t>(seed >> 24);
        }
        const std::size_t element = std::size_t{1} << texture->chain.layout->elementLog2;
        bool same = true;
        bool windowed = true;
        std::size_t unionFirst = bytes;
        std::size_t unionEnd = 0;
        for (std::uint32_t slice = 0; slice < texture->array && same && windowed; ++slice) {
            for (std::uint32_t mip = 0; mip < texture->chain.levels.size() && same && windowed; ++mip) {
                const MipLevel& level = texture->chain.levels[mip];
                const std::size_t rowBytes = level.width * element;
                std::vector<std::uint8_t> expected(rowBytes * level.height, 0);
                for (std::size_t y = 0; y < level.height; ++y) {
                    for (std::size_t x = 0; x < level.width; ++x) {
                        const std::size_t at = static_cast<std::size_t>(slice) * texture->chain.sliceBytes + texture->chain.ElementAt(level, x, y, slice);
                        if (at + element <= bytes) std::memcpy(expected.data() + y * rowBytes + x * element, tiled.data() + at, element);
                    }
                }
                std::vector<std::uint8_t> whole(expected.size(), 0xEE);
                same = DetileSubresource(*texture, tiled.data(), bytes, mip, slice, whole.data(), rowBytes, level.height, rowBytes, error) && whole == expected;
                const std::optional<std::pair<std::size_t, std::size_t>> range = SubresourceBytes(*texture, mip, slice);
                windowed = range.has_value();
                if (!range) break;
                const std::size_t first = (std::min)(range->first, bytes);
                const std::size_t end = (std::min)(range->second, bytes);
                unionFirst = (std::min)(unionFirst, first);
                unionEnd = (std::max)(unionEnd, end);
                std::vector<std::uint8_t> part(expected.size(), 0xEE);
                windowed = DetileSubresource(*texture, tiled.data() + first, end - first, mip, slice, part.data(), rowBytes, level.height, rowBytes, error, first) && part == expected;
            }
        }
        Expect(same, name + " detiles every mip as the element offsets say: " + error);
        Expect(windowed, name + " detiles every mip from only the bytes SubresourceBytes names: " + error);
        Expect(unionFirst < unionEnd && unionEnd <= bytes, name + " byte ranges lie inside the texel bytes");
    }
}

}

int main() {
    PatternsArePermutations();
    ThickPatternsArePermutations();
    RetailChainSizes();
    LevelsDoNotOverlap();
    TiledLevelsRoundTrip();
    VolumeLevelsRoundTrip();
    ImageDescriptorLayout();
    TileThenDetileWithPartialBlocks();
    LinearAndRowMajor();
    RetailSwizzleBits();
    ConsoleDescriptions();
    DetileMatchesElementOffsets();
    if (g_failures != 0) {
        std::printf("%d failures\n", g_failures);
        return 1;
    }
    std::printf("gfx10 tiling: all checks passed\n");
    return 0;
}
