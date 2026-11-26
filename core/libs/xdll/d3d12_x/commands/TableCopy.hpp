#pragma once

#include "VulkanDevice.hpp"

#include <cstdint>
#include <string>

namespace abx::d3d12x {

constexpr std::uint64_t kTableCopyBytes = 8ull << 20;

class TableCopy {
public:
    static TableCopy* For(const VulkanDevice& device);

    void Copy(VkCommandBuffer commands, std::uint64_t root, std::uint64_t destination, bool destinationIsDevice, std::uint64_t source, bool sourceIsDevice, std::uint64_t bytes) const;
    void Fill(VkCommandBuffer commands, std::uint64_t root, std::uint64_t destination, std::uint64_t bytes, std::uint32_t value) const;

private:
    explicit TableCopy(const VulkanDevice& device) : device_(device) {}
    bool Create(std::string& error);
    void Record(VkCommandBuffer commands, std::uint64_t root, std::uint64_t destination, std::uint64_t source, std::uint64_t bytes, std::uint32_t value, std::uint32_t mode) const;

    const VulkanDevice& device_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}
