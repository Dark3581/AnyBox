#pragma once

#include "VulkanDevice.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace abx::d3d12x {

constexpr std::uint64_t kTitlePageBytes = 64ull << 10;
constexpr std::uint64_t kTitleRegionBytes = 4ull << 30;
constexpr std::uint64_t kTitleRegionCount = 1ull << 15;
constexpr std::uint64_t kTitlePagesPerRegion = kTitleRegionBytes / kTitlePageBytes;

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
    bool Contains(std::uint64_t address, std::uint64_t bytes) const;
    struct Location {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
    };
    std::optional<Location> Locate(std::uint64_t address, std::uint64_t bytes) const;
    std::uint64_t ImportedBytes() const;
    std::optional<std::uint64_t> AliasBelow4GiB(std::uint64_t start, std::uint64_t bytes, std::string& error);
    void ReleaseAlias(std::uint64_t alias);

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
    };

    explicit TitleAddressSpace(const VulkanDevice& device) : device_(device) {}
    bool Initialize(std::string& error);
    bool CreateTable(std::uint64_t bytes, Allocation& table, std::string& error);
    bool ImportLocked(std::uint64_t start, std::uint64_t bytes, std::string& error);
    void Destroy(Allocation& allocation);
    std::uint64_t* RegionTable(std::uint64_t region, std::string& error);
    void Point(std::uint64_t start, std::uint64_t bytes, VkDeviceAddress device, std::uint64_t deviceStart);
    void Clear(std::uint64_t start, std::uint64_t bytes);
    void PointAliases();
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
};

}
