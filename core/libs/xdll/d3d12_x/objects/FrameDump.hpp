#pragma once

#include <cstdint>

namespace abx::d3d12x {

class Resource;
class VulkanDevice;

void NoteTracedTarget(Resource* target);
void NoteTracedTable(std::uint32_t gpu);
void DumpTracedFrame(const VulkanDevice& device, Resource* presented, std::uint64_t frame);

}
