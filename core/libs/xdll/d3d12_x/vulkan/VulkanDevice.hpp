#pragma once

#include "VulkanBackend.hpp"
#include "VulkanLoader.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
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
    bool presentation = false;
    bool pageableMemory = false;
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
    PFN_vkCmdFillBuffer cmdFillBuffer = nullptr;
    PFN_vkCmdUpdateBuffer cmdUpdateBuffer = nullptr;
    PFN_vkCmdCopyBufferToImage cmdCopyBufferToImage = nullptr;
    PFN_vkCmdCopyImageToBuffer cmdCopyImageToBuffer = nullptr;
    PFN_vkCmdCopyImage cmdCopyImage = nullptr;
    PFN_vkCmdResolveImage cmdResolveImage = nullptr;
    PFN_vkCmdSetViewportWithCount cmdSetViewportWithCount = nullptr;
    PFN_vkCmdSetScissorWithCount cmdSetScissorWithCount = nullptr;
    PFN_vkCmdSetPrimitiveTopology cmdSetPrimitiveTopology = nullptr;
    PFN_vkCmdSetPrimitiveRestartEnable cmdSetPrimitiveRestartEnable = nullptr;
    PFN_vkCmdSetPolygonModeEXT cmdSetPolygonMode = nullptr;
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
    PFN_vkCmdDrawMeshTasksEXT cmdDrawMeshTasks = nullptr;
    PFN_vkCmdDrawMeshTasksIndirectEXT cmdDrawMeshTasksIndirect = nullptr;
    PFN_vkCmdDrawMeshTasksIndirectCountEXT cmdDrawMeshTasksIndirectCount = nullptr;
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
    PFN_vkGetFenceStatus getFenceStatus = nullptr;
    PFN_vkGetBufferDeviceAddress getBufferDeviceAddress = nullptr;
    PFN_vkQueueWaitIdle queueWaitIdle = nullptr;
};

const std::vector<std::string>& KnownShaderFeatures();

struct RecordedCommandCounts {
    std::atomic<std::uint64_t> renderPasses{0};
    std::atomic<std::uint64_t> pipelineBarriers{0};
};

RecordedCommandCounts& RecordedCommands();

class VulkanDevice {
public:
    static std::unique_ptr<VulkanDevice> Create(const DeviceRequirements& requirements, std::string& error);
    ~VulkanDevice();
    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    VkDevice Handle() const { return device_; }
    VkInstance Instance() const { return instance_.Get(); }
    VkPhysicalDevice Physical() const { return physical_; }
    PFN_vkGetInstanceProcAddr InstanceLoader() const { return instanceLoader_; }
    PFN_vkGetDeviceProcAddr DeviceLoader() const { return deviceLoader_; }
    const DeviceFunctions& Vk() const { return vk_; }
    const GpuCapabilities& Gpu() const { return gpu_; }
    const VkPhysicalDeviceProperties& Properties() const { return properties_; }
    std::uint32_t QueueFamily() const { return queueFamily_; }
    VkPipelineLayout TitlePipelineLayout() const { return pipelineLayout_; }
    VkDescriptorSetLayout TitleSetLayout() const { return setLayout_; }
    VkQueue Queue() const { return queue_; }
    bool BufferDeviceAddress() const { return bufferDeviceAddress_; }
    bool RectangleFill() const { return rectangleFill_; }
    bool ConservativeRasterization() const { return conservative_; }
    const std::uint32_t* MeshMaxGroups() const { return meshMaxGroups_; }
    std::uint32_t MeshMaxTotal() const { return meshMaxTotal_; }
    std::uint32_t MeshPreferredLanes() const { return meshPreferredLanes_; }
    std::string FaultReport() const;
    std::string CheckpointReport() const;
    void Checkpoint(VkCommandBuffer commands, const char* marker) const;
    bool Checkpoints() const { return checkpoints_; }
    bool PageableMemory() const { return pageableMemory_; }
    void SetFaultAddressDescriber(std::function<std::string(std::uint64_t)> describer) { faultDescriber_ = std::move(describer); }
    void SetFaultExtraReport(std::function<std::string()> report) { faultExtra_ = std::move(report); }
    VkDeviceSize HostImportAlignment() const { return hostImportAlignment_; }
    PFN_vkGetMemoryHostPointerPropertiesEXT HostPointerProperties() const { return hostPointerProperties_; }
    std::optional<std::uint32_t> MemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags required) const;
    std::optional<std::uint32_t> SystemMemoryType(std::uint32_t typeBits) const;
    VkFormatFeatureFlags OptimalFeatures(VkFormat format) const;
    bool Submit(const std::function<void(VkCommandBuffer)>& record, std::string& error) const;
    struct PendingSubmit {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
    };
    std::optional<PendingSubmit> SubmitWithoutWaiting(const std::function<void(VkCommandBuffer)>& record, std::string& error) const;
    VkResult Finish(PendingSubmit& pending, bool wait) const;
    std::uint64_t SubmitDetached(const std::function<void(VkCommandBuffer)>& record, std::string& error) const;
    void WaitDetached(std::uint64_t serial) const;
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
    mutable std::mutex detachedLock_;
    mutable std::deque<PendingSubmit> detached_;
    mutable std::uint64_t detachedSubmitted_ = 0;
    mutable std::atomic<std::uint64_t> detachedFinished_{0};
    std::size_t validationErrors_ = 0;
    std::vector<std::string> validationMessages_;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr instanceLoader_ = nullptr;
    PFN_vkGetDeviceProcAddr deviceLoader_ = nullptr;
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
    std::uint32_t meshMaxGroups_[3] = {};
    std::uint32_t meshPreferredLanes_ = 0;
    std::uint32_t meshMaxTotal_ = 0;
    PFN_vkGetMemoryHostPointerPropertiesEXT hostPointerProperties_ = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties formatProperties_ = nullptr;
    bool subgroupSizeControl_ = false;
    bool pipelineCacheControl_ = false;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    std::vector<std::string> enabled_;
    bool rectangleFill_ = false;
    bool deviceFault_ = false;
    bool pageableMemory_ = false;
    bool conservative_ = false;
    PFN_vkGetDeviceFaultInfoEXT faultInfo_ = nullptr;
    bool checkpoints_ = false;
    PFN_vkCmdSetCheckpointNV setCheckpoint_ = nullptr;
    PFN_vkGetQueueCheckpointDataNV checkpointData_ = nullptr;
    std::function<std::string(std::uint64_t)> faultDescriber_;
    std::function<std::string()> faultExtra_;
    std::vector<std::string> extensions_;
};

}
