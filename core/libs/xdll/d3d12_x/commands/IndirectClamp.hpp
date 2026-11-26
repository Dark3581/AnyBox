#pragma once

#include "BindlessSet.hpp"
#include "VulkanDevice.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace abx::d3d12x {

struct ClampedArguments {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
};

class IndirectClamp {
public:
    static IndirectClamp* For(const VulkanDevice& device);
    static std::string RecentReport();

    std::optional<ClampedArguments> RecordDispatches(VkCommandBuffer commands, VkBuffer arguments, VkDeviceSize offset, std::uint32_t records, std::uint32_t stride, bool mesh,
                                                     std::string& error);
    void Trace(VkCommandBuffer commands, const ClampedArguments& checked, std::uint32_t records, std::uint32_t stride, bool mesh, VkBuffer count, VkDeviceSize countOffset,
               std::uint64_t titleAddress, std::string label);
    static void ReportTraced(std::uint64_t present);

private:
    explicit IndirectClamp(const VulkanDevice& device) : device_(device) {}
    bool Create(std::string& error);
    void Report();
    VkDeviceSize Reserve(VkDeviceSize room);

    static constexpr std::uint32_t kNotRun = 0xAB0D15EDu;
    static constexpr VkDeviceSize kHeaderBytes = 64;
    static constexpr VkDeviceSize kScratchBytes = 4u << 20;
    static constexpr std::uint32_t kDefaultMaxGroups = 1u << 22;

    const VulkanDevice& device_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    DeviceBuffer scratch_;
    std::uint32_t maxDimension_[3] = {};
    std::uint32_t meshMaxDimension_[3] = {};
    std::uint32_t meshMaxGroups_ = 0;
    std::uint32_t maxGroups_ = kDefaultMaxGroups;
    std::atomic<std::uint64_t> next_{0};
    std::atomic<std::uint32_t> reported_{0};
    struct Recent {
        VkDeviceSize at = 0;
        std::uint32_t records = 0;
        std::uint32_t strideWords = 0;
        bool mesh = false;
    };
    static constexpr std::size_t kRecent = 8;
    std::mutex recentLock_;
    std::array<Recent, kRecent> recent_{};
    std::uint64_t recentCount_ = 0;
    static constexpr std::size_t kRecentMesh = 256;
    std::array<Recent, kRecentMesh> recentMesh_{};
    std::uint64_t recentMeshCount_ = 0;
    struct Traced {
        VkDeviceSize at = 0;
        VkDeviceSize countAt = 0;
        std::uint32_t records = 0;
        std::uint32_t strideWords = 0;
        std::uint64_t titleAddress = 0;
        bool mesh = false;
        bool counted = false;
        bool waited = false;
        std::uint64_t waitedAt = 0;
        std::string label;
    };
    std::mutex tracedLock_;
    std::vector<Traced> traced_;
};

}
