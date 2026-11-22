#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace abx::d3d12x {

struct SubgroupCapabilities {
    std::uint32_t size = 0;
    std::uint32_t minSize = 0;
    std::uint32_t maxSize = 0;
    std::uint32_t stages = 0;
    std::uint32_t operations = 0;
    std::uint32_t requiredSizeStages = 0;
};

struct GpuCapabilities {
    std::string name;
    std::string type;
    std::uint32_t apiVersion = 0;
    std::uint32_t driverVersion = 0;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint32_t maxPushConstantsSize = 0;
    SubgroupCapabilities subgroup;
    std::map<std::string, bool> features;
    std::vector<std::string> extensions;
};

struct VulkanReport {
    bool available = false;
    std::string error;
    std::uint32_t instanceVersion = 0;
    std::vector<GpuCapabilities> devices;
};

VulkanReport QueryVulkan();
std::string VersionText(std::uint32_t version);
std::string ToJson(const VulkanReport& report);

}
