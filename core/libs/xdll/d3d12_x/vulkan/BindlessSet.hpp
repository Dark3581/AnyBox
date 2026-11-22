#pragma once

#include "DescriptorTable.hpp"
#include "TitleLayout.hpp"
#include "VulkanDevice.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace abx::d3d12x {

struct DeviceBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
    VkDeviceAddress address = 0;
};

bool CreateHostBuffer(const VulkanDevice& device, VkDeviceSize size, VkBufferUsageFlags usage, DeviceBuffer& out, std::string& error);
void DestroyBuffer(const VulkanDevice& device, DeviceBuffer& buffer);

class BindlessSet {
public:
    static std::unique_ptr<BindlessSet> Create(const VulkanDevice& device, std::string& error);
    ~BindlessSet();
    BindlessSet(const BindlessSet&) = delete;
    BindlessSet& operator=(const BindlessSet&) = delete;

    VkDescriptorSet Set() const { return set_; }
    DescriptorHashTable& Table() { return table_; }
    const DescriptorHashTable& Table() const { return table_; }
    bool Upload(std::string& error);
    bool WriteImage(std::uint32_t binding, std::uint32_t index, VkImageView view, std::string& error);
    bool WriteSampler(std::uint32_t index, VkSampler sampler, std::string& error);
    std::size_t FallbackImages() const { return fallbacks_.size(); }

private:
    struct Fallback {
        ImageDim dim = ImageDim::None;
        bool arrayed = false;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    explicit BindlessSet(const VulkanDevice& device) : device_(device) {}
    bool Initialize(std::string& error);
    Fallback* FallbackFor(const BindingSpec& spec, std::string& error);
    void WriteTableBinding();

    const VulkanDevice& device_;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkSampler fallbackSampler_ = VK_NULL_HANDLE;
    std::vector<Fallback> fallbacks_;
    DeviceBuffer tableBuffer_;
    DescriptorHashTable table_;
    std::uint64_t uploaded_ = ~0ull;
};

}
