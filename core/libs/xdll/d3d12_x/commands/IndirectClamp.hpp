#pragma once

#include "BindlessSet.hpp"
#include "VulkanDevice.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

namespace abx::d3d12x {

struct ClampedArguments {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
};

class IndirectClamp {
public:
    static IndirectClamp* For(const VulkanDevice& device);

    std::optional<ClampedArguments> RecordDispatches(VkCommandBuffer commands, VkBuffer arguments, VkDeviceSize offset, std::uint32_t records, std::uint32_t stride, bool mesh,
                                                     std::string& error);

private:
    explicit IndirectClamp(const VulkanDevice& device) : device_(device) {}
    bool Create(std::string& error);
    void Report();

    static constexpr VkDeviceSize kHeaderBytes = 64;
    static constexpr VkDeviceSize kScratchBytes = 4u << 20;
    static constexpr std::uint32_t kMaxGroups = 1u << 22;

    const VulkanDevice& device_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    DeviceBuffer scratch_;
    std::uint32_t maxDimension_[3] = {};
    std::uint32_t meshMaxDimension_[3] = {};
    std::uint32_t meshMaxGroups_ = 0;
    std::atomic<std::uint64_t> next_{0};
    std::atomic<std::uint32_t> reported_{0};
};

}
