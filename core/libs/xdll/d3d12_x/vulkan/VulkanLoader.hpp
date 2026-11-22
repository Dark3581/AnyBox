#pragma once

#include "VulkanBackend.hpp"

#include <windows.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

namespace abx::d3d12x {

class VulkanLibrary {
public:
    VulkanLibrary() : module_(LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {}
    ~VulkanLibrary() {
        if (module_ != nullptr) {
            FreeLibrary(module_);
        }
    }
    VulkanLibrary(const VulkanLibrary&) = delete;
    VulkanLibrary& operator=(const VulkanLibrary&) = delete;

    PFN_vkGetInstanceProcAddr Loader() const {
        return module_ == nullptr ? nullptr
                                  : reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(module_, "vkGetInstanceProcAddr")));
    }

private:
    HMODULE module_;
};

class VulkanInstance {
public:
    VulkanInstance() = default;
    VulkanInstance(VkInstance instance, PFN_vkDestroyInstance destroy) : instance_(instance), destroy_(destroy) {}
    ~VulkanInstance() { Reset(); }
    VulkanInstance(const VulkanInstance&) = delete;
    VulkanInstance& operator=(const VulkanInstance&) = delete;

    VkInstance Get() const { return instance_; }
    void Assign(VkInstance instance, PFN_vkDestroyInstance destroy) {
        Reset();
        instance_ = instance;
        destroy_ = destroy;
    }
    void Reset() {
        if (instance_ != VK_NULL_HANDLE && destroy_ != nullptr) {
            destroy_(instance_, nullptr);
        }
        instance_ = VK_NULL_HANDLE;
    }

private:
    VkInstance instance_ = VK_NULL_HANDLE;
    PFN_vkDestroyInstance destroy_ = nullptr;
};

template <typename Function>
Function LoadInstanceFunction(PFN_vkGetInstanceProcAddr loader, VkInstance instance, const char* name) {
    return reinterpret_cast<Function>(loader(instance, name));
}

GpuCapabilities DescribePhysicalDevice(PFN_vkGetInstanceProcAddr loader, VkInstance instance, VkPhysicalDevice device);

}
