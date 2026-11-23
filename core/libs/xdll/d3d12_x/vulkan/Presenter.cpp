#include "Presenter.hpp"

#include <vulkan/vulkan_win32.h>

#include <algorithm>

namespace abx::d3d12x {

namespace {

constexpr std::uint64_t kNoTimeout = ~std::uint64_t{0};

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
    if (process != GetCurrentProcessId() || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) return TRUE;
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
    const VkImage target = images_[index];
    const VkExtent2D window = extent_;
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

}
