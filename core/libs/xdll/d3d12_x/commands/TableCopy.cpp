#include "TableCopy.hpp"

#include "AbxApi.hpp"

#include <mutex>

namespace abx::d3d12x {

namespace {

const std::uint32_t kTableCopySpirv[] = {
#include "TableCopySpirv.inc"
};

constexpr std::uint32_t kLanes = 64;
constexpr std::uint32_t kSourceIsDevice = 1;
constexpr std::uint32_t kDestinationIsDevice = 2;
constexpr std::uint32_t kFill = 4;

struct TablePush {
    std::uint64_t root;
    std::uint64_t destination;
    std::uint64_t source;
    std::uint32_t words;
    std::uint32_t value;
    std::uint32_t mode;
    std::uint32_t spare;
};

void Barrier(const VulkanDevice& device, VkCommandBuffer commands, VkPipelineStageFlags2 fromStage, VkAccessFlags2 fromAccess, VkPipelineStageFlags2 toStage, VkAccessFlags2 toAccess) {
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = fromStage;
    barrier.srcAccessMask = fromAccess;
    barrier.dstStageMask = toStage;
    barrier.dstAccessMask = toAccess;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    device.Vk().cmdPipelineBarrier2(commands, &dependency);
}

std::mutex g_tableCopyLock;
TableCopy* g_tableCopy = nullptr;
bool g_tableCopyTried = false;

}

TableCopy* TableCopy::For(const VulkanDevice& device) {
    const std::lock_guard guard(g_tableCopyLock);
    if (g_tableCopyTried) return g_tableCopy;
    g_tableCopyTried = true;
    auto* copy = new TableCopy(device);
    std::string error;
    if (!copy->Create(error)) {
        AbxLog("d3d12_x", "copies and fills of title memory cannot run through the page table on this device, they name the memory as a buffer: %s", error.c_str());
        delete copy;
        return nullptr;
    }
    g_tableCopy = copy;
    return g_tableCopy;
}

bool TableCopy::Create(std::string& error) {
    const DeviceFunctions& vk = device_.Vk();
    if (!device_.BufferDeviceAddress()) {
        error = "the device has no buffer device addresses";
        return false;
    }
    if (device_.Properties().limits.maxComputeWorkGroupCount[0] < kTableCopyBytes / 4 / kLanes) {
        error = "the device runs too few compute groups in one dispatch";
        return false;
    }
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = sizeof(kTableCopySpirv);
    moduleInfo.pCode = kTableCopySpirv;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vk.createShaderModule(device_.Handle(), &moduleInfo, nullptr, &module) != VK_SUCCESS) {
        error = "vkCreateShaderModule failed for the copy through the page table";
        return false;
    }
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TablePush)};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vk.createPipelineLayout(device_.Handle(), &layoutInfo, nullptr, &layout_) != VK_SUCCESS) {
        vk.destroyShaderModule(device_.Handle(), module, nullptr);
        error = "vkCreatePipelineLayout failed for the copy through the page table";
        return false;
    }
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = layout_;
    const VkResult result = vk.createComputePipelines(device_.Handle(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
    vk.destroyShaderModule(device_.Handle(), module, nullptr);
    if (result != VK_SUCCESS) {
        pipeline_ = VK_NULL_HANDLE;
        error = "vkCreateComputePipelines failed for the copy through the page table with " + std::to_string(static_cast<int>(result));
        return false;
    }
    return true;
}

void TableCopy::Record(VkCommandBuffer commands, std::uint64_t root, std::uint64_t destination, std::uint64_t source, std::uint64_t bytes, std::uint32_t value, std::uint32_t mode) const {
    if (bytes == 0 || bytes % 4 != 0 || bytes > kTableCopyBytes || destination % 4 != 0 || source % 4 != 0) {
        AbxUnsupported("ID3D12GraphicsCommandList", "a copy through the page table that is empty, larger than 8 MiB or not 4-byte aligned");
    }
    const DeviceFunctions& vk = device_.Vk();
    TablePush push{};
    push.root = root;
    push.destination = destination;
    push.source = source;
    push.words = static_cast<std::uint32_t>(bytes / 4);
    push.value = value;
    push.mode = mode;
    Barrier(device_, commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vk.cmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vk.cmdPushConstants(commands, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vk.cmdDispatch(commands, (push.words + kLanes - 1) / kLanes, 1, 1);
    Barrier(device_, commands, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}

void TableCopy::Copy(VkCommandBuffer commands, std::uint64_t root, std::uint64_t destination, bool destinationIsDevice, std::uint64_t source, bool sourceIsDevice,
                     std::uint64_t bytes) const {
    Record(commands, root, destination, source, bytes, 0, (destinationIsDevice ? kDestinationIsDevice : 0u) | (sourceIsDevice ? kSourceIsDevice : 0u));
}

void TableCopy::Fill(VkCommandBuffer commands, std::uint64_t root, std::uint64_t destination, std::uint64_t bytes, std::uint32_t value) const {
    Record(commands, root, destination, 0, bytes, value, kFill);
}

}
