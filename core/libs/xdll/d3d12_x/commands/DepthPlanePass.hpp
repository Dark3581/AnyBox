#pragma once

#include "VulkanDevice.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace abx::d3d12x {

class DepthPlanePass {
public:
    struct Source {
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };

    static DepthPlanePass* For(const VulkanDevice& device);

    bool CreateSource(VkImageView values, Source& source, std::string& error) const;
    void DestroySource(Source& source) const;
    VkPipeline Pipeline(VkFormat depthFormat, bool stencil, std::string& error);
    void Record(VkCommandBuffer commands, VkPipeline pipeline, const Source& source, VkImageView depth, bool stencil, VkExtent2D extent, bool setFixedState) const;

private:
    explicit DepthPlanePass(const VulkanDevice& device) : device_(device) {}
    bool Create(std::string& error);

    const VulkanDevice& device_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vertex_ = VK_NULL_HANDLE;
    VkShaderModule fragment_ = VK_NULL_HANDLE;
    std::mutex lock_;
    std::map<std::pair<VkFormat, bool>, VkPipeline> pipelines_;
};

}
