#include "Presenter.hpp"

#include "FrameTiming.hpp"

#include <vulkan/vulkan_win32.h>
#include <string_view>

#include <algorithm>
#include <mutex>
#include <optional>
#include <cstdint>

namespace abx::d3d12x {

namespace {

constexpr std::uint64_t kNoTimeout = ~std::uint64_t{0};

const std::uint32_t kCompositeSpirv[] = {
#include "PlaneCompositeSpirv.inc"
};

template <typename Function>
Function Instance(const VulkanDevice& device, const char* name) {
    return reinterpret_cast<Function>(device.InstanceLoader()(device.Instance(), name));
}

template <typename Function>
Function Device(const VulkanDevice& device, const char* name) {
    return reinterpret_cast<Function>(device.DeviceLoader()(device.Handle(), name));
}

BOOL CALLBACK FindWindowOfProcess(HWND window, LPARAM found) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr || window == GetConsoleWindow()) return TRUE;
    wchar_t className[64] = {};
    GetClassNameW(window, className, 64);
    if (std::wstring_view(className) == L"ConsoleWindowClass" || std::wstring_view(className) == L"PseudoConsoleWindow") return TRUE;
    *reinterpret_cast<HWND*>(found) = window;
    return FALSE;
}

VkImageMemoryBarrier2 ImageBarrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags2 fromAccess, VkAccessFlags2 toAccess) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = fromAccess;
    barrier.dstAccessMask = toAccess;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return barrier;
}

}

HWND FindProcessWindow() {
    HWND found = nullptr;
    EnumWindows(&FindWindowOfProcess, reinterpret_cast<LPARAM>(&found));
    return found;
}

std::unique_ptr<Presenter> Presenter::Create(const VulkanDevice& device, HWND window, std::string& error) {
    std::unique_ptr<Presenter> presenter(new Presenter(device, window));
    if (!presenter->Initialize(error)) return nullptr;
    return presenter;
}

Presenter::~Presenter() {
    DestroyComposite();
    const DeviceFunctions& vk = device_.Vk();
    if (pipeline_ != VK_NULL_HANDLE) vk.destroyPipeline(device_.Handle(), pipeline_, nullptr);
    if (pipelineLayout_ != VK_NULL_HANDLE) vk.destroyPipelineLayout(device_.Handle(), pipelineLayout_, nullptr);
    if (pool_ != VK_NULL_HANDLE) vk.destroyDescriptorPool(device_.Handle(), pool_, nullptr);
    if (setLayout_ != VK_NULL_HANDLE) vk.destroyDescriptorSetLayout(device_.Handle(), setLayout_, nullptr);
    if (sampler_ != VK_NULL_HANDLE) vk.destroySampler(device_.Handle(), sampler_, nullptr);
    DestroySwapchain();
    if (acquired_ != VK_NULL_HANDLE) destroyFence_(device_.Handle(), acquired_, nullptr);
    if (surface_ != VK_NULL_HANDLE) destroySurface_(device_.Instance(), surface_, nullptr);
}

bool Presenter::Initialize(std::string& error) {
    if (device_.InstanceLoader() == nullptr || device_.DeviceLoader() == nullptr) {
        error = "the Vulkan device has no loaders";
        return false;
    }
    const auto createSurface = Instance<PFN_vkCreateWin32SurfaceKHR>(device_, "vkCreateWin32SurfaceKHR");
    destroySurface_ = Instance<PFN_vkDestroySurfaceKHR>(device_, "vkDestroySurfaceKHR");
    surfaceSupport_ = Instance<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(device_, "vkGetPhysicalDeviceSurfaceSupportKHR");
    surfaceCapabilities_ = Instance<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(device_, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    surfaceFormats_ = Instance<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(device_, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    createSwapchain_ = Device<PFN_vkCreateSwapchainKHR>(device_, "vkCreateSwapchainKHR");
    destroySwapchain_ = Device<PFN_vkDestroySwapchainKHR>(device_, "vkDestroySwapchainKHR");
    swapchainImages_ = Device<PFN_vkGetSwapchainImagesKHR>(device_, "vkGetSwapchainImagesKHR");
    acquireNextImage_ = Device<PFN_vkAcquireNextImageKHR>(device_, "vkAcquireNextImageKHR");
    queuePresent_ = Device<PFN_vkQueuePresentKHR>(device_, "vkQueuePresentKHR");
    cmdBlitImage_ = Device<PFN_vkCmdBlitImage>(device_, "vkCmdBlitImage");
    createFence_ = Device<PFN_vkCreateFence>(device_, "vkCreateFence");
    destroyFence_ = Device<PFN_vkDestroyFence>(device_, "vkDestroyFence");
    waitForFences_ = Device<PFN_vkWaitForFences>(device_, "vkWaitForFences");
    resetFences_ = Device<PFN_vkResetFences>(device_, "vkResetFences");
    if (createSurface == nullptr || destroySurface_ == nullptr || surfaceSupport_ == nullptr || surfaceCapabilities_ == nullptr || surfaceFormats_ == nullptr ||
        createSwapchain_ == nullptr || destroySwapchain_ == nullptr || swapchainImages_ == nullptr || acquireNextImage_ == nullptr || queuePresent_ == nullptr ||
        cmdBlitImage_ == nullptr || createFence_ == nullptr || destroyFence_ == nullptr || waitForFences_ == nullptr || resetFences_ == nullptr) {
        error = "the Vulkan device was created without presentation support";
        return false;
    }
    VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surfaceInfo.hinstance = GetModuleHandleW(nullptr);
    surfaceInfo.hwnd = window_;
    if (createSurface(device_.Instance(), &surfaceInfo, nullptr, &surface_) != VK_SUCCESS) {
        surface_ = VK_NULL_HANDLE;
        error = "vkCreateWin32SurfaceKHR failed";
        return false;
    }
    VkBool32 supported = VK_FALSE;
    if (surfaceSupport_(device_.Physical(), device_.QueueFamily(), surface_, &supported) != VK_SUCCESS || !supported) {
        error = "the queue family cannot present to the window";
        return false;
    }
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (createFence_(device_.Handle(), &fenceInfo, nullptr, &acquired_) != VK_SUCCESS) {
        acquired_ = VK_NULL_HANDLE;
        error = "vkCreateFence failed";
        return false;
    }
    return CreateSwapchain(error);
}

void Presenter::DestroySwapchain() {
    if (swapchain_ != VK_NULL_HANDLE) destroySwapchain_(device_.Handle(), swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    images_.clear();
}

bool Presenter::CreateSwapchain(std::string& error) {
    VkSurfaceCapabilitiesKHR capabilities{};
    if (surfaceCapabilities_(device_.Physical(), surface_, &capabilities) != VK_SUCCESS) {
        error = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed";
        return false;
    }
    std::uint32_t formatCount = 0;
    surfaceFormats_(device_.Physical(), surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    surfaceFormats_(device_.Physical(), surface_, &formatCount, formats.data());
    if (formats.empty()) {
        error = "the surface offers no formats";
        return false;
    }
    VkSurfaceFormatKHR chosen = formats.front();
    for (const VkSurfaceFormatKHR& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM) {
            chosen = format;
            break;
        }
    }
    if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0) {
        error = "the swapchain images cannot be blit targets";
        return false;
    }
    extent_ = capabilities.currentExtent;
    if (extent_.width == 0xFFFFFFFFu) {
        RECT client{};
        GetClientRect(window_, &client);
        extent_ = {static_cast<std::uint32_t>((std::max)(client.right - client.left, LONG{1})), static_cast<std::uint32_t>((std::max)(client.bottom - client.top, LONG{1}))};
    }
    if (extent_.width == 0 || extent_.height == 0) return true;
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_;
    info.minImageCount = (std::max)(capabilities.minImageCount, 2u);
    if (capabilities.maxImageCount != 0) info.minImageCount = (std::min)(info.minImageCount, capabilities.maxImageCount);
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    if (createSwapchain_(device_.Handle(), &info, nullptr, &swapchain_) != VK_SUCCESS) {
        swapchain_ = VK_NULL_HANDLE;
        error = "vkCreateSwapchainKHR failed";
        return false;
    }
    format_ = chosen.format;
    std::uint32_t imageCount = 0;
    swapchainImages_(device_.Handle(), swapchain_, &imageCount, nullptr);
    images_.resize(imageCount);
    swapchainImages_(device_.Handle(), swapchain_, &imageCount, images_.data());
    return true;
}

bool Presenter::Present(VkImage source, VkExtent2D extent, VkImageLayout layout, std::string& error) {
    if (swapchain_ == VK_NULL_HANDLE) {
        if (!CreateSwapchain(error)) return false;
        if (swapchain_ == VK_NULL_HANDLE) return true;
    }
    std::uint32_t index = 0;
    const auto acquireBegin = std::chrono::steady_clock::now();
    VkResult acquired = acquireNextImage_(device_.Handle(), swapchain_, kNoTimeout, VK_NULL_HANDLE, acquired_, &index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        DestroySwapchain();
        return true;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        error = "vkAcquireNextImageKHR failed with " + std::to_string(static_cast<int>(acquired));
        return false;
    }
    waitForFences_(device_.Handle(), 1, &acquired_, VK_TRUE, kNoTimeout);
    resetFences_(device_.Handle(), 1, &acquired_);
    AddTime(TimedPart::PresentAcquire, std::chrono::steady_clock::now() - acquireBegin);
    const VkImage target = images_[index];
    const VkExtent2D window = extent_;
    const TimedScope timedSync(TimedPart::PresentSync);
    const bool submitted = device_.Submit(
        [&](VkCommandBuffer commands) {
            const VkImageMemoryBarrier2 toTransfer = ImageBarrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkDependencyInfo first{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            first.imageMemoryBarrierCount = 1;
            first.pImageMemoryBarriers = &toTransfer;
            device_.Vk().cmdPipelineBarrier2(commands, &first);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {static_cast<std::int32_t>(extent.width), static_cast<std::int32_t>(extent.height), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[1] = {static_cast<std::int32_t>(window.width), static_cast<std::int32_t>(window.height), 1};
            cmdBlitImage_(commands, source, layout, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            const VkImageMemoryBarrier2 toPresent =
                ImageBarrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0);
            VkDependencyInfo second{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            second.imageMemoryBarrierCount = 1;
            second.pImageMemoryBarriers = &toPresent;
            device_.Vk().cmdPipelineBarrier2(commands, &second);
        },
        error);
    if (!submitted) return false;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &index;
    VkResult presented = VK_SUCCESS;
    {
        const std::lock_guard guard(device_.QueueLock());
        presented = queuePresent_(device_.Queue(), &present);
    }
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        DestroySwapchain();
        return true;
    }
    if (presented != VK_SUCCESS) {
        error = "vkQueuePresentKHR failed with " + std::to_string(static_cast<int>(presented));
        return false;
    }
    return true;
}

bool Presenter::Acquire(std::uint32_t& index, bool& skipped, std::string& error) {
    skipped = false;
    if (swapchain_ == VK_NULL_HANDLE) {
        if (!CreateSwapchain(error)) return false;
        if (swapchain_ == VK_NULL_HANDLE) {
            skipped = true;
            return true;
        }
    }
    const TimedScope timedAcquire(TimedPart::PresentAcquire);
    const VkResult acquired = acquireNextImage_(device_.Handle(), swapchain_, kNoTimeout, VK_NULL_HANDLE, acquired_, &index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        DestroySwapchain();
        skipped = true;
        return true;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        error = "vkAcquireNextImageKHR failed with " + std::to_string(static_cast<int>(acquired));
        return false;
    }
    waitForFences_(device_.Handle(), 1, &acquired_, VK_TRUE, kNoTimeout);
    resetFences_(device_.Handle(), 1, &acquired_);
    return true;
}

bool Presenter::QueueImage(std::uint32_t index, std::string& error) {
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &index;
    VkResult presented = VK_SUCCESS;
    {
        const std::lock_guard guard(device_.QueueLock());
        presented = queuePresent_(device_.Queue(), &present);
    }
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        DestroySwapchain();
        return true;
    }
    if (presented != VK_SUCCESS) {
        error = "vkQueuePresentKHR failed with " + std::to_string(static_cast<int>(presented));
        return false;
    }
    return true;
}

bool Presenter::CreateCompositor(std::string& error) {
    if (pipeline_ != VK_NULL_HANDLE) return true;
    const DeviceFunctions& vk = device_.Vk();
    const VkDevice device = device_.Handle();
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vk.createSampler(device, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) {
        error = "vkCreateSampler failed for the plane compositor";
        return false;
    }
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = 3;
    setInfo.pBindings = bindings;
    if (vk.createDescriptorSetLayout(device, &setInfo, nullptr, &setLayout_) != VK_SUCCESS) {
        error = "vkCreateDescriptorSetLayout failed for the plane compositor";
        return false;
    }
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(std::uint32_t)};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vk.createPipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        error = "vkCreatePipelineLayout failed for the plane compositor";
        return false;
    }
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    if (vk.createDescriptorPool(device, &poolInfo, nullptr, &pool_) != VK_SUCCESS) {
        error = "vkCreateDescriptorPool failed for the plane compositor";
        return false;
    }
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &setLayout_;
    if (vk.allocateDescriptorSets(device, &allocate, &set_) != VK_SUCCESS) {
        error = "vkAllocateDescriptorSets failed for the plane compositor";
        return false;
    }
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = sizeof(kCompositeSpirv);
    moduleInfo.pCode = kCompositeSpirv;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vk.createShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
        error = "vkCreateShaderModule failed for the plane compositor";
        return false;
    }
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = pipelineLayout_;
    const VkResult created = vk.createComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_);
    vk.destroyShaderModule(device, module, nullptr);
    if (created != VK_SUCCESS) {
        pipeline_ = VK_NULL_HANDLE;
        error = "vkCreateComputePipelines failed for the plane compositor with " + std::to_string(static_cast<int>(created));
        return false;
    }
    return true;
}

void Presenter::DestroyComposite() {
    const DeviceFunctions& vk = device_.Vk();
    if (compositeView_ != VK_NULL_HANDLE) vk.destroyImageView(device_.Handle(), compositeView_, nullptr);
    if (composite_ != VK_NULL_HANDLE) vk.destroyImage(device_.Handle(), composite_, nullptr);
    if (compositeMemory_ != VK_NULL_HANDLE) vk.freeMemory(device_.Handle(), compositeMemory_, nullptr);
    compositeView_ = VK_NULL_HANDLE;
    composite_ = VK_NULL_HANDLE;
    compositeMemory_ = VK_NULL_HANDLE;
    compositeExtent_ = {};
}

bool Presenter::EnsureComposite(std::string& error) {
    if (composite_ != VK_NULL_HANDLE && compositeExtent_.width == extent_.width && compositeExtent_.height == extent_.height) return true;
    DestroyComposite();
    const DeviceFunctions& vk = device_.Vk();
    const VkDevice device = device_.Handle();
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {extent_.width, extent_.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk.createImage(device, &info, nullptr, &composite_) != VK_SUCCESS) {
        composite_ = VK_NULL_HANDLE;
        error = "vkCreateImage failed for the composited frame";
        return false;
    }
    VkMemoryRequirements needs{};
    vk.getImageMemoryRequirements(device, composite_, &needs);
    const std::optional<std::uint32_t> type = device_.MemoryType(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryPriorityAllocateInfoEXT priority{VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT};
    priority.priority = 0.75f;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    if (device_.PageableMemory()) allocate.pNext = &priority;
    allocate.allocationSize = needs.size;
    allocate.memoryTypeIndex = type.value_or(0);
    if (!type || vk.allocateMemory(device, &allocate, nullptr, &compositeMemory_) != VK_SUCCESS || vk.bindImageMemory(device, composite_, compositeMemory_, 0) != VK_SUCCESS) {
        error = "no device memory for the composited frame";
        DestroyComposite();
        return false;
    }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = composite_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vk.createImageView(device, &view, nullptr, &compositeView_) != VK_SUCCESS) {
        compositeView_ = VK_NULL_HANDLE;
        error = "vkCreateImageView failed for the composited frame";
        DestroyComposite();
        return false;
    }
    compositeExtent_ = extent_;
    return true;
}

bool Presenter::PresentComposited(const Plane& scene, const Plane& overlay, VkImageLayout layout, bool premultiplied, std::string& error) {
    if (!CreateCompositor(error)) return false;
    std::uint32_t index = 0;
    bool skipped = false;
    if (!Acquire(index, skipped, error)) return false;
    if (skipped) return true;
    if (!EnsureComposite(error)) return false;
    const DeviceFunctions& vk = device_.Vk();
    const VkDevice device = device_.Handle();
    VkImageView views[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    const Plane planes[2] = {scene, overlay};
    for (int plane = 0; plane < 2; ++plane) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = planes[plane].image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = planes[plane].format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vk.createImageView(device, &view, nullptr, &views[plane]) != VK_SUCCESS) {
            views[plane] = VK_NULL_HANDLE;
            for (VkImageView made : views) {
                if (made != VK_NULL_HANDLE) vk.destroyImageView(device, made, nullptr);
            }
            error = "vkCreateImageView failed for a present plane";
            return false;
        }
    }
    const VkDescriptorImageInfo images[3] = {{sampler_, views[0], layout}, {sampler_, views[1], layout}, {VK_NULL_HANDLE, compositeView_, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[3]{};
    for (std::uint32_t binding = 0; binding < 3; ++binding) {
        writes[binding] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[binding].dstSet = set_;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType = binding == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[binding].pImageInfo = &images[binding];
    }
    vk.updateDescriptorSets(device, 3, writes, 0, nullptr);
    const VkImage target = images_[index];
    const VkExtent2D window = extent_;
    const std::uint32_t blend = premultiplied ? 1u : 0u;
    const auto syncBegin = std::chrono::steady_clock::now();
    const bool submitted = device_.Submit(
        [&](VkCommandBuffer commands) {
            VkMemoryBarrier2 written{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            written.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            written.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            written.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            written.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            const VkImageMemoryBarrier2 toStorage = ImageBarrier(composite_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            VkDependencyInfo before{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            before.memoryBarrierCount = 1;
            before.pMemoryBarriers = &written;
            before.imageMemoryBarrierCount = 1;
            before.pImageMemoryBarriers = &toStorage;
            vk.cmdPipelineBarrier2(commands, &before);
            vk.cmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
            vk.cmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &set_, 0, nullptr);
            vk.cmdPushConstants(commands, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(blend), &blend);
            vk.cmdDispatch(commands, (window.width + 7) / 8, (window.height + 7) / 8, 1);
            const VkImageMemoryBarrier2 middle[2] = {
                ImageBarrier(composite_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT),
                ImageBarrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_2_TRANSFER_WRITE_BIT),
            };
            VkDependencyInfo between{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            between.imageMemoryBarrierCount = 2;
            between.pImageMemoryBarriers = middle;
            vk.cmdPipelineBarrier2(commands, &between);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {static_cast<std::int32_t>(window.width), static_cast<std::int32_t>(window.height), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[1] = {static_cast<std::int32_t>(window.width), static_cast<std::int32_t>(window.height), 1};
            cmdBlitImage_(commands, composite_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
            const VkImageMemoryBarrier2 toPresent =
                ImageBarrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0);
            VkDependencyInfo after{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            after.imageMemoryBarrierCount = 1;
            after.pImageMemoryBarriers = &toPresent;
            vk.cmdPipelineBarrier2(commands, &after);
        },
        error);
    AddTime(TimedPart::PresentSync, std::chrono::steady_clock::now() - syncBegin);
    for (VkImageView made : views) vk.destroyImageView(device, made, nullptr);
    if (!submitted) return false;
    return QueueImage(index, error);
}

}
