#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace abx::d3d12x {

class Resource;
class TitleAddressSpace;
class VulkanDevice;

void NoteTracedTarget(Resource* target);
void NoteTracedDepth(Resource* target);
void NoteTracedInput(Resource* input);
void NoteTracedRead(Resource* read);
void NoteWatchedRead(Resource* read);
bool WatchedShader(const std::string& shaders);
void NoteTracedTable(std::uint32_t gpu);
void DumpTracedBuffers(const TitleAddressSpace* addresses, const std::string& shader, unsigned list, const std::uint32_t* words, std::size_t count);
void DumpTracedFrame(const VulkanDevice& device, Resource* presented, std::uint64_t frame);
void PollFrameTraceRequest();

}
