#include "VulkanBackend.hpp"

#include <cstdio>

int main() {
    const abx::d3d12x::VulkanReport report = abx::d3d12x::QueryVulkan();
    std::fputs(abx::d3d12x::ToJson(report).c_str(), stdout);
    return report.available ? 0 : 3;
}
