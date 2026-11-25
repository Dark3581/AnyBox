#pragma once

#include "VulkanDevice.hpp"

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace abx::d3d12x {

class Presenter {
public:
    static std::unique_ptr<Presenter> Create(const VulkanDevice& device, HWND window, std::string& error);
    ~Presenter();
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    bool Present(VkImage source, VkExtent2D extent, VkImageLayout layout, std::string& error);

private:
    explicit Presenter(const VulkanDevice& device, HWND window) : device_(device), window_(window) {}
    bool Initialize(std::string& error);
    bool CreateSwapchain(std::string& error);
    void DestroySwapchain();

    const VulkanDevice& device_;
    HWND window_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFence acquired_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> images_;
    PFN_vkDestroySurfaceKHR destroySurface_ = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR surfaceSupport_ = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR surfaceCapabilities_ = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR surfaceFormats_ = nullptr;
    PFN_vkCreateSwapchainKHR createSwapchain_ = nullptr;
    PFN_vkDestroySwapchainKHR destroySwapchain_ = nullptr;
    PFN_vkGetSwapchainImagesKHR swapchainImages_ = nullptr;
    PFN_vkAcquireNextImageKHR acquireNextImage_ = nullptr;
    PFN_vkQueuePresentKHR queuePresent_ = nullptr;
    PFN_vkCmdBlitImage cmdBlitImage_ = nullptr;
    PFN_vkCreateFence createFence_ = nullptr;
    PFN_vkDestroyFence destroyFence_ = nullptr;
    PFN_vkWaitForFences waitForFences_ = nullptr;
    PFN_vkResetFences resetFences_ = nullptr;
};

HWND FindProcessWindow();

}
