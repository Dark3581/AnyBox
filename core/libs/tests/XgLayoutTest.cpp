#include "Gfx10Tiling.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
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

D3D12_RESOURCE_DESC Desc(std::uint64_t width, std::uint32_t height, std::uint16_t array, std::uint16_t mips, std::uint32_t format, std::uint32_t layout, std::uint32_t flags = 0) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = array;
    desc.MipLevels = mips;
    desc.Format = static_cast<DXGI_FORMAT>(format);
    desc.SampleDesc.Count = 1;
    desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(layout);
    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(flags);
    return desc;
}

std::optional<ConsoleLayout> Describe(const D3D12_RESOURCE_DESC& desc, HRESULT* code = nullptr) {
    Refusal refusal;
    std::optional<ConsoleLayout> layout = DescribeConsoleLayout(desc, kSeriesXPipes, refusal);
    if (code != nullptr) *code = layout ? S_OK : refusal.code;
    return layout;
}

std::vector<std::pair<std::uint32_t, std::uint32_t>> UsagesAndBytes(const ConsoleLayout& layout) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
    for (const Plane& plane : layout.planes) {
        const Surface* surface = std::get_if<Surface>(&plane.kind);
        out.emplace_back(plane.usage, surface != nullptr ? surface->bytesPerElement : 0u);
    }
    return out;
}

std::uint64_t Load64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::uint32_t Load32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::vector<std::uint8_t> WriteLayout(const ConsoleLayout& layout) {
    std::vector<std::uint8_t> bytes(kResourceLayoutBytes, 0xCD);
    layout.Write(std::span<std::uint8_t, kResourceLayoutBytes>(bytes.data(), kResourceLayoutBytes));
    return bytes;
}

std::vector<std::uint8_t> Noise(std::size_t bytes, std::uint32_t& state) {
    std::vector<std::uint8_t> out(bytes);
    for (std::uint8_t& value : out) {
        state = state * 1103515245u + 12345u;
        value = static_cast<std::uint8_t>((state >> 16) | 1u);
    }
    return out;
}

void Rgba8In64KbS() {
    const std::optional<ConsoleLayout> layout = Describe(Desc(256, 256, 1, 0, 28, 0x109));
    Expect(layout.has_value(), "RGBA8 256x256 in 64KB_S has a layout");
    if (!layout) return;
    const std::vector<std::uint8_t> b = WriteLayout(*layout);
    Expect(Load64(b, 0) == 0x60000 && Load64(b, 8) == 0x10000 && Load32(b, 0x10) == 9, "size 0x60000, alignment 0x10000, 9 mips");
    Expect(Load32(b, kLayoutPlaneBase + 0x2C) == 2 && Load32(b, kLayoutPlaneBase + 0x30) == 7, "mip tail from mip 2, 7 mips in it");
    const auto mip = [&](std::size_t i, std::size_t k) { return Load32(b, kLayoutPlaneBase + kLayoutMipBase + i * kLayoutMipBytes + 4 * k); };
    const std::uint32_t offsets[] = {0x20000, 0x10000, 0x8000, 0x4000, 0x2000, 0x1000, 0x800, 0x600, 0x500};
    for (std::size_t i = 0; i < 9; ++i) Expect(mip(i, 2) == offsets[i], "mip " + std::to_string(i) + " offset " + std::to_string(mip(i, 2)));
    Expect(mip(2, 0) == 0x8000 && mip(2, 9) == 0x40 && mip(2, 10) == 0x80 && mip(2, 18) == 1 && mip(6, 9) == 8 && mip(6, 10) == 8, "tail levels' size, padded size and mode");
    Expect(Load32(b, kLayoutDimension) == 3, "dimension 3 at 0x1B38");
    Expect(OptimalMode(0, 0, 32, 256, 256) == 5, "a sampled 256x256 RGBA8 texture is 4KB_S");
}

void D16S8IsPlanarDepthAndStencil() {
    const std::optional<ConsoleLayout> buffer = Describe(Desc(1920, 1080, 1, 1, 118, 0, 2));
    Expect(buffer.has_value(), "D16S8 depth buffer has a layout");
    if (buffer) {
        const std::vector<std::pair<std::uint32_t, std::uint32_t>> want{{4, 0}, {7, 2}, {8, 1}};
        Expect(UsagesAndBytes(*buffer) == want, "D16S8 depth buffer: HTILE, 2-byte depth, 1-byte stencil");
        std::uint64_t end = 0;
        bool ordered = true;
        for (std::size_t k = 0; k < buffer->planes.size(); ++k) {
            end = (std::max)(end, buffer->planes[k].offset + buffer->planes[k].Bytes());
            if (k + 1 < buffer->planes.size()) ordered = ordered && buffer->planes[k].offset + buffer->planes[k].Bytes() <= buffer->planes[k + 1].offset;
        }
        Expect(ordered, "D16S8 planes do not overlap");
        Expect(buffer->Size() == end, "D16S8 size is the end of its last plane");
        Expect(buffer->Size() >= 1920ull * 1080 * 3 && buffer->Size() < 1920ull * 1080 * 8, "D16S8 size " + std::to_string(buffer->Size()));
        Expect(buffer->Alignment() == 0x10000, "D16S8 depth buffer alignment 64 KiB");
        std::string error;
        const std::optional<ConsoleTexture> texture = DescribeConsoleTexture(Desc(1920, 1080, 1, 1, 118, 0, 2), kSeriesXPipes, error);
        Expect(texture && texture->texelOffset == buffer->planes[1].offset && texture->alignment == 0x10000 && texture->size == buffer->Size(),
               "ConsoleTexture of the D16S8 depth buffer: depth plane offset, alignment and size: " + error);
    }
    const std::optional<ConsoleLayout> plain = Describe(Desc(512, 512, 1, 1, 118, 0));
    Expect(plain.has_value(), "D16S8 texture has a layout");
    if (plain) {
        const std::vector<std::pair<std::uint32_t, std::uint32_t>> want{{5, 2}, {6, 1}};
        Expect(UsagesAndBytes(*plain) == want, "D16S8 texture: depth and stencil planes only");
        Expect(plain->Size() >= 512ull * 512 * 3 && plain->Size() != (std::numeric_limits<std::uint64_t>::max)(), "D16S8 texture size " + std::to_string(plain->Size()));
    }
}

void ConsoleFloatsAre32Bits() {
    const std::optional<ConsoleLayout> rgb10a2 = Describe(Desc(1920, 1080, 1, 1, 24, 0));
    for (const std::uint32_t format : {116u, 117u}) {
        const std::optional<ConsoleLayout> layout = Describe(Desc(1920, 1080, 1, 1, format, 0));
        const std::vector<std::pair<std::uint32_t, std::uint32_t>> want{{1, 4}};
        Expect(layout && rgb10a2 && UsagesAndBytes(*layout) == want && layout->Size() == rgb10a2->Size() && layout->Alignment() == rgb10a2->Alignment(),
               "format " + std::to_string(format) + " is laid out as R10G10B10A2");
    }
}

void SuggestedLayouts() {
    Refusal fallback;
    Expect(SuggestSwizzleLayout(Desc(256, 256, 1, 0, 28, 0x109), kSeriesXPipes, fallback) == 0x105 && fallback.code == S_OK, "a sampled texture: 4KB_S, its own layout ignored");
    Expect(SuggestSwizzleLayout(Desc(256, 256, 1, 1, 28, 0, 1), kSeriesXPipes, fallback) == 0x11B, "a render target: 64KB_R_X");
    Expect(SuggestSwizzleLayout(Desc(256, 256, 1, 1, 40, 0, 2), kSeriesXPipes, fallback) == 0x118, "a depth buffer: 64KB_Z_X");
    Expect(SuggestSwizzleLayout(Desc(128, 128, 1, 1, 28, 0, 4), kSeriesXPipes, fallback) == 0x115, "a small UAV: 4KB_S_X");
    Expect(SuggestSwizzleLayout(Desc(1024, 1024, 1, 1, 28, 0, 4), kSeriesXPipes, fallback) == 0x11B, "a large UAV: 64KB_R_X");
    D3D12_RESOURCE_DESC line = Desc(256, 1, 1, 1, 28, 0);
    line.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    Expect(SuggestSwizzleLayout(line, kSeriesXPipes, fallback) == 0x100, "a 1D texture: linear");
    D3D12_RESOURCE_DESC msaa = Desc(256, 256, 1, 1, 28, 0, 1);
    msaa.SampleDesc.Count = 4;
    Refusal msaaFallback;
    Expect(SuggestSwizzleLayout(msaa, kSeriesXPipes, msaaFallback) == 0x11B && msaaFallback.code == E_NOTIMPL, "a multisampled render target: the choice by use, with the refusal reported");
}

void Refusals() {
    HRESULT code = S_OK;
    Expect(!Describe(Desc(256, 256, 1, 1, 28, 0x11C), &code) && code == E_NOTIMPL, "VAR mode 28: E_NOTIMPL");
    Expect(!Describe(Desc(256, 256, 1, 1, 6, 0), &code) && code == E_INVALIDARG, "a 96-bit format: E_INVALIDARG");
    Expect(!Describe(Desc(256, 256, 1, 16, 28, 0), &code) && code == E_INVALIDARG, "16 mips: E_INVALIDARG");
    Expect(!Describe(Desc(256, 256, 1, 1, 40, 0x105, 2), &code) && code == E_INVALIDARG, "a depth buffer outside 64KB_Z_X: E_INVALIDARG");
    Expect(!Describe(Desc(256, 256, 1, 1, 20, 2), &code) && code == E_INVALIDARG, "a stencil format with the 64KB_D choice: E_INVALIDARG");
    D3D12_RESOURCE_DESC buffer = Desc(65536, 1, 1, 1, 0, 1);
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    Expect(!Describe(buffer, &code) && code == E_NOTIMPL, "a buffer: E_NOTIMPL");
    D3D12_RESOURCE_DESC volume = Desc(64, 64, 8, 1, 28, 2);
    volume.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    Expect(!Describe(volume, &code) && code == E_NOTIMPL, "a 3D texture with the 64KB_D choice: E_NOTIMPL");
    const std::optional<ConsoleLayout> layout = Describe(Desc(64, 64, 1, 1, 28, 0x109));
    std::vector<std::uint8_t> memory(layout ? layout->Size() : 0);
    std::vector<std::uint8_t> rows(64 * 256);
    std::string error;
    Expect(layout && CopyIntoSubresource(*layout, memory.data(), 1, 0, rows.data(), 256, 0, error) == E_INVALIDARG, "copy into a plane the texture has not: E_INVALIDARG");
    Expect(layout && CopyIntoSubresource(*layout, memory.data(), 0, 1, rows.data(), 256, 0, error) == E_INVALIDARG, "copy into a subresource past the texture: E_INVALIDARG");
    Expect(layout && CopyFromSubresource(*layout, nullptr, 0, 0, memory.data(), 256, 0, error) == E_INVALIDARG, "copy to null: E_INVALIDARG");
}

void CopiesRoundTrip() {
    const std::optional<ConsoleLayout> layout = Describe(Desc(96, 40, 3, 5, 28, 0x11B));
    Expect(layout.has_value(), "RGBA8 96x40x3 in 64KB_R_X has a layout");
    if (!layout) return;
    const Surface& surface = *layout->Texels();
    std::vector<std::uint8_t> memory(layout->Size());
    std::vector<std::vector<std::uint8_t>> sources;
    std::uint32_t state = 3;
    bool copied = true;
    const std::uint32_t rowPitch = 512;
    for (std::uint32_t sub = 0; sub < 5 * 3; ++sub) {
        const MipLevel& level = surface.chain.levels[sub % 5];
        std::vector<std::uint8_t> source = Noise(rowPitch * level.height, state);
        std::string error;
        copied = CopyIntoSubresource(*layout, memory.data(), 0, sub, source.data(), rowPitch, 0, error) == S_OK && copied;
        sources.push_back(std::move(source));
    }
    Expect(copied, "every subresource copies in");
    bool same = true;
    bool addressed = true;
    for (std::uint32_t sub = 0; sub < 5 * 3; ++sub) {
        const std::uint32_t mip = sub % 5;
        const std::uint32_t slice = sub / 5;
        const MipLevel& level = surface.chain.levels[mip];
        std::vector<std::uint8_t> back(rowPitch * level.height, 0);
        std::string error;
        same = CopyFromSubresource(*layout, back.data(), 0, sub, memory.data(), rowPitch, 0, error) == S_OK && same;
        for (std::size_t y = 0; y < level.height; ++y) {
            same = same && std::memcmp(back.data() + y * rowPitch, sources[sub].data() + y * rowPitch, level.width * 4) == 0;
            for (std::size_t x = 0; x < level.width; ++x) {
                const std::uint64_t at = layout->ElementOffset(mip, x, y, slice);
                addressed = addressed && at + 4 <= memory.size() && std::memcmp(memory.data() + at, sources[sub].data() + y * rowPitch + x * 4, 4) == 0;
            }
        }
    }
    Expect(same, "every subresource copies back out unchanged");
    Expect(addressed, "every texel is where GetTexelElementOffsetBytes says");

    D3D12_RESOURCE_DESC volumeDesc = Desc(32, 32, 8, 3, 28, 0x105);
    volumeDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    const std::optional<ConsoleLayout> volume = Describe(volumeDesc);
    Expect(volume && volume->Texels() && volume->Texels()->chain.layout->blockDepth > 1 && volume->dimension == 4, "RGBA8 32x32x8 volume in thick 4KB_S");
    if (!volume) return;
    std::vector<std::uint8_t> volumeMemory(volume->Size());
    const std::uint32_t slicePitch = 128 * 32;
    bool volumeSame = true;
    for (std::uint32_t mip = 0; mip < 3; ++mip) {
        const MipLevel& level = volume->Texels()->chain.levels[mip];
        std::vector<std::uint8_t> source = Noise(slicePitch * level.depth, state);
        std::vector<std::uint8_t> back(source.size(), 0);
        std::string error;
        volumeSame = CopyIntoSubresource(*volume, volumeMemory.data(), 0, mip, source.data(), 128, slicePitch, error) == S_OK && volumeSame;
        volumeSame = CopyFromSubresource(*volume, back.data(), 0, mip, volumeMemory.data(), 128, slicePitch, error) == S_OK && volumeSame;
        for (std::size_t z = 0; z < level.depth; ++z) {
            for (std::size_t y = 0; y < level.height; ++y) {
                const std::size_t row = z * slicePitch + y * 128;
                volumeSame = volumeSame && std::memcmp(back.data() + row, source.data() + row, level.width * 4) == 0;
            }
        }
    }
    Expect(volumeSame, "every volume level copies in and back out unchanged");
}

void ResourceTilings() {
    const std::optional<ConsoleLayout> layout = Describe(Desc(256, 256, 1, 0, 28, 0x109));
    if (!layout) {
        Expect(false, "RGBA8 256x256 in 64KB_S has a layout");
        return;
    }
    UINT total = 0;
    D3D12_PACKED_MIP_INFO packed{};
    D3D12_TILE_SHAPE shape{};
    UINT count = 9;
    std::array<D3D12_SUBRESOURCE_TILING, 9> tilings{};
    std::string error;
    Expect(ResourceTiling(*layout, &total, &packed, &shape, &count, 0, tilings.data(), error) == S_OK, "64KB_S tiling: " + error);
    Expect(total == 6, "6 tiles of 64 KiB");
    Expect(packed.NumStandardMips == 2 && packed.NumPackedMips == 7 && packed.NumTilesForPackedMips == 1 && packed.StartTileIndexInOverallResource == 0, "2 standard mips, 7 packed in one tile");
    Expect(shape.WidthInTexels == 128 && shape.HeightInTexels == 128 && shape.DepthInTexels == 1, "128x128 texel tiles");
    Expect(count == 2, "the standard subresources among the 9 asked");
    Expect(tilings[0].WidthInTiles == 2 && tilings[0].HeightInTiles == 2 && tilings[0].DepthInTiles == 1 && tilings[0].StartTileIndexInOverallResource == 2, "mip 0: 2x2 tiles from tile 2");
    Expect(tilings[1].WidthInTiles == 1 && tilings[1].HeightInTiles == 1 && tilings[1].StartTileIndexInOverallResource == 1, "mip 1: one tile, tile 1");
    Expect(tilings[2].WidthInTiles == 0 && tilings[2].StartTileIndexInOverallResource == 0, "packed mips left as they were");
    const std::optional<ConsoleLayout> smallTiles = Describe(Desc(256, 256, 1, 1, 28, 0x105));
    Expect(smallTiles && ResourceTiling(*smallTiles, &total, nullptr, nullptr, nullptr, 0, nullptr, error) == E_FAIL, "4KB_S tiling: E_FAIL");
    D3D12_RESOURCE_DESC volumeDesc = Desc(64, 64, 64, 1, 28, 0x109);
    volumeDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    const std::optional<ConsoleLayout> volume = Describe(volumeDesc);
    Expect(volume && ResourceTiling(*volume, &total, nullptr, nullptr, nullptr, 0, nullptr, error) == E_NOTIMPL, "volume tiling: E_NOTIMPL");
}

}

int main() {
    Rgba8In64KbS();
    D16S8IsPlanarDepthAndStencil();
    ConsoleFloatsAre32Bits();
    SuggestedLayouts();
    Refusals();
    CopiesRoundTrip();
    ResourceTilings();
    if (g_failures != 0) {
        std::printf("%d failures\n", g_failures);
        return 1;
    }
    std::printf("xg layout: all checks passed\n");
    return 0;
}
