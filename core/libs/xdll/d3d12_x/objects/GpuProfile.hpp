#pragma once

#include "VulkanDevice.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace abx::d3d12x {

class GpuProfile {
public:
    static void Start(const VulkanDevice& device);
    static std::uint32_t BeginList(VkCommandBuffer commands, std::uint64_t& list);
    static void MarkWork(VkCommandBuffer commands, std::uint32_t generation, std::uint64_t list, const char* function, const std::vector<std::string>& shaders);
    static void EndList(VkCommandBuffer commands, std::uint32_t generation, std::uint64_t list);
    static void FramePresented(std::uint64_t frame);
};

}
