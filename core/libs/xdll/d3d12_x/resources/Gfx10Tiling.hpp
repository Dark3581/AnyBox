#pragma once

#include <windows.h>
#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace abx::d3d12x::gfx10 {

struct PipeConfig {
    std::uint32_t pipesLog2 = 0;
    std::uint32_t packersLog2 = 0;
};

constexpr PipeConfig kSeriesXPipes{5, 3};
constexpr PipeConfig kSeriesSPipes{3, 2};

inline PipeConfig ConsolePipes(const char* model) {
    if (model != nullptr && model[0] == 's' && model[1] == 'e' && model[2] == 'r' && model[3] == 'i' && model[4] == 'e' && model[5] == 's' && model[6] == '_' && model[7] == 's' && model[8] == 0) {
        return kSeriesSPipes;
    }
    return kSeriesXPipes;
}

constexpr std::uint32_t kMaxMips = 15;
constexpr std::size_t kResourceLayoutBytes = 0x1B40;
constexpr std::size_t kLayoutPlaneBase = 0x18;
constexpr std::size_t kLayoutMipBase = 0x38;
constexpr std::size_t kLayoutMipBytes = 0x70;
constexpr std::size_t kLayoutPlaneBytes = kLayoutMipBase + kMaxMips * kLayoutMipBytes;
constexpr std::size_t kLayoutDimension = 0x1B38;
constexpr std::uint64_t kTiledResourceTileBytes = 0x10000;

struct Layout {
    std::uint32_t mode = 0;
    std::uint32_t elementLog2 = 0;
    std::size_t blockWidth = 1;
    std::size_t blockHeight = 1;
    std::size_t blockDepth = 1;
    std::size_t blockBytes = 256;
    std::size_t baseXor = 0;
    std::vector<std::uint32_t> ox, oy, oz, hx, hy, hz;

    std::pair<std::size_t, std::size_t> Blocks(std::size_t width, std::size_t height) const;
    std::size_t Size(std::size_t width, std::size_t height) const;
    std::size_t Offset(std::size_t x, std::size_t y, std::size_t z, std::size_t width) const;
    std::vector<std::uint8_t> Detile(std::span<const std::uint8_t> tiled, std::size_t width, std::size_t height) const;
    bool Tile(std::span<const std::uint8_t> linear, std::size_t width, std::size_t height, std::vector<std::uint8_t>& tiled) const;
};

std::shared_ptr<const Layout> Layout2d(std::uint32_t mode, std::uint32_t elementLog2, PipeConfig pipes);
std::shared_ptr<const Layout> Layout3d(std::uint32_t mode, std::uint32_t elementLog2, PipeConfig pipes);

struct MipLevel {
    std::size_t width = 0;
    std::size_t height = 0;
    std::size_t depth = 1;
    std::size_t offset = 0;
    std::size_t pitch = 0;
    std::size_t originX = 0;
    std::size_t originY = 0;
    std::optional<std::size_t> tail;
};

struct Chain {
    std::shared_ptr<const Layout> layout;
    std::vector<MipLevel> levels;
    std::size_t sliceBytes = 0;

    static Chain Make(std::shared_ptr<const Layout> layout, std::uint32_t width, std::uint32_t height, std::uint32_t depth, std::uint32_t side, std::uint32_t mips);
    std::size_t Layers() const;
    std::size_t ElementAt(const MipLevel& level, std::size_t x, std::size_t y, std::size_t slice) const;
    std::size_t ElementAtZ(const MipLevel& level, std::size_t x, std::size_t y, std::size_t z) const;
    std::vector<std::uint8_t> DetileLevel(std::span<const std::uint8_t> slice, std::size_t mip, std::size_t sliceIndex) const;
    bool TileLevel(std::span<std::uint8_t> slice, std::size_t mip, std::size_t sliceIndex, std::span<const std::uint8_t> linear) const;
    std::vector<std::uint8_t> DetileVolume(std::span<const std::uint8_t> memory, std::size_t mip) const;
    bool TileVolume(std::span<std::uint8_t> memory, std::size_t mip, std::span<const std::uint8_t> linear) const;
};

struct ImageSurface {
    Layout layout;
    std::size_t width = 0;
    std::size_t height = 0;
};

std::optional<std::pair<std::uint32_t, std::uint32_t>> ImageFormatElement(std::uint32_t imageFormat);
std::optional<ImageSurface> ImageLayout(std::uint32_t imageFormat, std::uint32_t swizzleMode, std::uint64_t address, std::uint32_t width, std::uint32_t height, PipeConfig pipes);

struct Surface {
    Chain chain;
    std::uint32_t mode = 0;
    std::uint32_t bytesPerElement = 0;
    std::uint32_t side = 1;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t array = 1;
    std::uint32_t depth = 0;
    std::size_t firstTail = 0;

    std::uint64_t Size() const;
    std::uint64_t Alignment() const;
    std::pair<std::uint32_t, std::uint32_t> Slices(std::size_t mip) const;
    std::pair<std::size_t, std::size_t> Padded(std::size_t mip) const;
    std::uint64_t MipOffset(std::size_t mip) const;
    std::uint64_t ElementOffset(std::size_t mip, std::size_t x, std::size_t y, std::size_t slice) const;
    void WritePlane(std::span<std::uint8_t, kResourceLayoutBytes> layout, std::size_t planeBase) const;
};

struct Metadata {
    std::uint64_t bytes = 0;
    std::uint64_t alignment = 0;
};

struct Blank {};

constexpr std::uint32_t kUsageColor = 1;
constexpr std::uint32_t kUsageCmask = 2;
constexpr std::uint32_t kUsageHtile = 4;
constexpr std::uint32_t kUsageDepth = 5;
constexpr std::uint32_t kUsageStencil = 6;
constexpr std::uint32_t kUsageDepthBufferDepth = 7;
constexpr std::uint32_t kUsageDepthBufferStencil = 8;

struct Plane {
    std::uint32_t usage = 0;
    std::uint64_t offset = 0;
    std::variant<Surface, Metadata, Blank> kind = Blank{};

    std::uint64_t Bytes() const;
    std::uint64_t Alignment() const;
};

struct PlaneView {
    const Surface* surface = nullptr;
    std::uint64_t offset = 0;
};

struct ConsoleLayout {
    std::vector<Plane> planes;
    std::size_t main = 0;
    std::uint32_t mips = 1;
    bool empty = false;
    std::uint32_t dimension = 0;

    std::uint64_t Size() const;
    std::uint64_t Alignment() const;
    std::optional<PlaneView> PlaneSurface(std::uint32_t plane) const;
    const Surface* Texels() const;
    std::uint64_t MipOffset(std::size_t mip) const;
    std::uint64_t ElementOffset(std::size_t mip, std::size_t x, std::size_t y, std::size_t slice) const;
    void Write(std::span<std::uint8_t, kResourceLayoutBytes> layout) const;
};

struct Refusal {
    HRESULT code = S_OK;
    std::string reason;
};

std::optional<std::pair<std::uint32_t, std::uint32_t>> FormatElement(std::uint32_t format);
std::uint32_t OptimalMode(std::uint32_t layout, std::uint32_t flags, std::uint64_t bitsPerPixel, std::uint64_t width, std::uint32_t height);
std::optional<ConsoleLayout> DescribeConsoleLayout(const D3D12_RESOURCE_DESC& desc, PipeConfig pipes, Refusal& refusal);
std::uint32_t SuggestSwizzleLayout(const D3D12_RESOURCE_DESC& desc, PipeConfig pipes, Refusal& fallback);

HRESULT CopyIntoSubresource(const ConsoleLayout& layout, std::uint8_t* resource, std::uint32_t plane, std::uint32_t subresource, const std::uint8_t* source, std::uint32_t rowPitch,
                            std::uint32_t slicePitch, std::string& error);
HRESULT CopyFromSubresource(const ConsoleLayout& layout, std::uint8_t* destination, std::uint32_t plane, std::uint32_t subresource, const std::uint8_t* resource, std::uint32_t rowPitch,
                            std::uint32_t slicePitch, std::string& error);
HRESULT ResourceTiling(const ConsoleLayout& layout, UINT* totalTiles, D3D12_PACKED_MIP_INFO* packed, D3D12_TILE_SHAPE* shape, UINT* count, UINT first, D3D12_SUBRESOURCE_TILING* tilings,
                       std::string& error);

struct ConsoleTexture {
    Chain chain;
    std::uint32_t mode = 0;
    std::uint32_t bytesPerElement = 0;
    std::uint32_t side = 1;
    std::uint32_t array = 1;
    std::uint32_t depth = 0;
    std::uint32_t mips = 1;
    std::uint64_t texelOffset = 0;
    std::uint64_t texelBytes = 0;
    std::uint64_t size = 0;
    std::uint64_t alignment = 0;
};

std::optional<ConsoleTexture> DescribeConsoleTexture(const D3D12_RESOURCE_DESC& desc, PipeConfig pipes, std::string& error);

bool DetileSubresource(const ConsoleTexture& texture, const std::uint8_t* texels, std::size_t texelBytes, std::uint32_t mip, std::uint32_t slice, std::uint8_t* destination,
                       std::size_t rowPitch, std::size_t rows, std::size_t rowBytes, std::string& error, std::size_t firstByte = 0);
std::optional<std::pair<std::size_t, std::size_t>> SubresourceBytes(const ConsoleTexture& texture, std::uint32_t mip, std::uint32_t slice);

}
