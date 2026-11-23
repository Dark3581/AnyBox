#pragma once

#include "VulkanBackend.hpp"
#include "VulkanLoader.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace abx::d3d12x {

struct DeviceRequirements {
    std::set<std::string> features;
    bool maintenance8 = false;
    bool validation = false;
    bool hostMemoryImport = false;
    std::string preferredDevice;
};

int DeviceScore(VkPhysicalDeviceType type, VkDriverId driver, const std::string& name, const std::string& preferred);

struct DeviceFunctions {
    PFN_vkDestroyDevice destroyDevice = nullptr;
    PFN_vkGetDeviceQueue getDeviceQueue = nullptr;
    PFN_vkCreateShaderModule createShaderModule = nullptr;
    PFN_vkDestroyShaderModule destroyShaderModule = nullptr;
    PFN_vkCreateComputePipelines createComputePipelines = nullptr;
    PFN_vkCreateGraphicsPipelines createGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkCreatePipelineCache createPipelineCache = nullptr;
    PFN_vkDestroyPipelineCache destroyPipelineCache = nullptr;
    PFN_vkGetPipelineCacheData getPipelineCacheData = nullptr;
    PFN_vkCreateDescriptorSetLayout createDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout createPipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout destroyPipelineLayout = nullptr;
    PFN_vkCreateBuffer createBuffer = nullptr;
    PFN_vkDestroyBuffer destroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindBufferMemory bindBufferMemory = nullptr;
    PFN_vkMapMemory mapMemory = nullptr;
    PFN_vkUnmapMemory unmapMemory = nullptr;
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
    PFN_vkBindImageMemory bindImageMemory = nullptr;
    PFN_vkCreateImageView createImageView = nullptr;
    PFN_vkDestroyImageView destroyImageView = nullptr;
    PFN_vkCreateSampler createSampler = nullptr;
    PFN_vkDestroySampler destroySampler = nullptr;
    PFN_vkCreateDescriptorPool createDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool destroyDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets allocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets updateDescriptorSets = nullptr;
    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkResetCommandPool resetCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    PFN_vkCmdPipelineBarrier2 cmdPipelineBarrier2 = nullptr;
    PFN_vkCmdBindPipeline cmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets cmdBindDescriptorSets = nullptr;
    PFN_vkCmdPushConstants cmdPushConstants = nullptr;
    PFN_vkCmdDispatch cmdDispatch = nullptr;
    PFN_vkCmdClearColorImage cmdClearColorImage = nullptr;
    PFN_vkCmdClearDepthStencilImage cmdClearDepthStencilImage = nullptr;
    PFN_vkCmdBeginRendering cmdBeginRendering = nullptr;
    PFN_vkCmdEndRendering cmdEndRendering = nullptr;
    PFN_vkCmdClearAttachments cmdClearAttachments = nullptr;
    PFN_vkCmdCopyBuffer cmdCopyBuffer = nullptr;
    PFN_vkCmdCopyBufferToImage cmdCopyBufferToImage = nullptr;
    PFN_vkCmdCopyImageToBuffer cmdCopyImageToBuffer = nullptr;
    PFN_vkCmdCopyImage cmdCopyImage = nullptr;
    PFN_vkCmdResolveImage cmdResolveImage = nullptr;
    PFN_vkCmdSetViewportWithCount cmdSetViewportWithCount = nullptr;
    PFN_vkCmdSetScissorWithCount cmdSetScissorWithCount = nullptr;
    PFN_vkCmdSetPrimitiveTopology cmdSetPrimitiveTopology = nullptr;
    PFN_vkCmdSetPrimitiveRestartEnable cmdSetPrimitiveRestartEnable = nullptr;
    PFN_vkCmdSetBlendConstants cmdSetBlendConstants = nullptr;
    PFN_vkCmdSetStencilReference cmdSetStencilReference = nullptr;
    PFN_vkCmdBindVertexBuffers2 cmdBindVertexBuffers2 = nullptr;
    PFN_vkCmdBindIndexBuffer cmdBindIndexBuffer = nullptr;
    PFN_vkCmdDraw cmdDraw = nullptr;
    PFN_vkCmdDrawIndexed cmdDrawIndexed = nullptr;
    PFN_vkCmdDrawIndirect cmdDrawIndirect = nullptr;
    PFN_vkCmdDrawIndexedIndirect cmdDrawIndexedIndirect = nullptr;
    PFN_vkCmdDrawIndirectCount cmdDrawIndirectCount = nullptr;
    PFN_vkCmdDrawIndexedIndirectCount cmdDrawIndexedIndirectCount = nullptr;
    PFN_vkCmdDispatchIndirect cmdDispatchIndirect = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkCreateSemaphore createSemaphore = nullptr;
    PFN_vkDestroySemaphore destroySemaphore = nullptr;
    PFN_vkGetSemaphoreCounterValue getSemaphoreCounterValue = nullptr;
    PFN_vkSignalSemaphore signalSemaphore = nullptr;
    PFN_vkWaitSemaphores waitSemaphores = nullptr;
    PFN_vkQueueSubmit2 queueSubmit2 = nullptr;
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkDestroyFence destroyFence = nullptr;
    PFN_vkWaitForFences waitForFences = nullptr;
    PFN_vkGetBufferDeviceAddress getBufferDeviceAddress = nullptr;
};

const std::vector<std::string>& KnownShaderFeatures();

class VulkanDevice {
public:
    static std::unique_ptr<VulkanDevice> Create(const DeviceRequirements& requirements, std::string& error);
    ~VulkanDevice();
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    VkDevice Handle() const { return device_; }
    const DeviceFunctions& Vk() const { return vk_; }
    const GpuCapabilities& Gpu() const { return gpu_; }
    const VkPhysicalDeviceProperties& Properties() const { return properties_; }
    std::uint32_t QueueFamily() const { return queueFamily_; }
    VkPipelineLayout TitlePipelineLayout() const { return pipelineLayout_; }
    VkDescriptorSetLayout TitleSetLayout() const { return setLayout_; }
    VkQueue Queue() const { return queue_; }
    bool BufferDeviceAddress() const { return bufferDeviceAddress_; }
    VkDeviceSize HostImportAlignment() const { return hostImportAlignment_; }
    PFN_vkGetMemoryHostPointerPropertiesEXT HostPointerProperties() const { return hostPointerProperties_; }
    std::optional<std::uint32_t> MemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags required) const;
    bool Submit(const std::function<void(VkCommandBuffer)>& record, std::string& error) const;
    std::mutex& QueueLock() const { return queueLock_; }
    const std::vector<std::string>& EnabledFeatures() const { return enabled_; }
    bool PreferenceMissed() const { return preferenceMissed_; }
    bool HasFeature(const std::string& name) const;
    const std::vector<std::string>& EnabledExtensions() const { return extensions_; }
    bool CanRequireSubgroupSize(std::uint32_t size, VkShaderStageFlagBits stage) const;
    bool PipelineCacheControl() const { return pipelineCacheControl_; }
    std::size_t ValidationErrors() const;
    std::vector<std::string> ValidationMessages() const;

private:
    VulkanDevice() = default;
    bool Open(const DeviceRequirements& requirements, std::string& error);
    bool CreateLayout(std::string& error);
    static VKAPI_ATTR VkBool32 VKAPI_CALL OnMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                                                    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);

    VulkanLibrary library_;
    VulkanInstance instance_;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger_ = nullptr;
    mutable std::mutex messagesLock_;
    mutable std::mutex queueLock_;
    std::size_t validationErrors_ = 0;
    std::vector<std::string> validationMessages_;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    DeviceFunctions vk_;
    GpuCapabilities gpu_;
    VkPhysicalDeviceProperties properties_{};
    VkPhysicalDeviceVulkan12Properties limits12_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    std::uint32_t queueFamily_ = 0;
    bool preferenceMissed_ = false;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    bool bufferDeviceAddress_ = false;
    VkDeviceSize hostImportAlignment_ = 0;
    PFN_vkGetMemoryHostPointerPropertiesEXT hostPointerProperties_ = nullptr;
    bool subgroupSizeControl_ = false;
    bool pipelineCacheControl_ = false;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    std::vector<std::string> enabled_;
    std::vector<std::string> extensions_;
};

}
