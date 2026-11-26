#include "IndirectClamp.hpp"

#include "AbxApi.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace abx::d3d12x {

namespace {

const std::uint32_t kClampSpirv[] = {
#include "IndirectClampSpirv.inc"
};

struct ClampPush {
    std::uint64_t arguments;
    std::uint64_t stats;
    std::uint32_t records;
    std::uint32_t strideWords;
    std::uint32_t maxX;
    std::uint32_t maxY;
    std::uint32_t maxZ;
    std::uint32_t maxGroups;
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

std::mutex g_clampLock;
IndirectClamp* g_clamp = nullptr;

}

IndirectClamp* IndirectClamp::For(const VulkanDevice& device) {
    static IndirectClamp*& created = g_clamp;
    static bool tried = false;
    const std::lock_guard guard(g_clampLock);
    if (tried) return created;
    tried = true;
    const char* setting = AbxSetting("d3d12_x.clamp_indirect_dispatches");
    if (setting != nullptr && setting[0] == '0') {
        AbxLog("d3d12_x", "indirect dispatch arguments are not checked before the GPU reads them (d3d12_x.clamp_indirect_dispatches = 0)");
        return nullptr;
    }
    auto* clamp = new IndirectClamp(device);
    std::string error;
    if (!clamp->Create(error)) {
        AbxLog("d3d12_x", "indirect dispatch arguments cannot be checked on this device: %s", error.c_str());
        delete clamp;
        return nullptr;
    }
    AbxLog("d3d12_x", "indirect dispatch arguments are copied and checked on the GPU before they are used: a record asking for more than %u x %u x %u groups (the device's limits, "
                      "each capped at the total) or %u in all runs empty, as counts left by work AnyBox skipped would otherwise run until Windows resets the GPU "
                      "(d3d12_x.clamp_indirect_dispatches = 0 turns this off)",
           clamp->maxDimension_[0], clamp->maxDimension_[1], clamp->maxDimension_[2], clamp->maxGroups_);
    if (clamp->maxGroups_ != kDefaultMaxGroups) {
        AbxLog("d3d12_x", "indirect compute and mesh records that ask for more than %u groups in all run empty (d3d12_x.indirect_groups_max; %u unless set)", clamp->maxGroups_,
               kDefaultMaxGroups);
    }
    created = clamp;
    return created;
}

bool IndirectClamp::Create(std::string& error) {
    const DeviceFunctions& vk = device_.Vk();
    if (!device_.BufferDeviceAddress()) {
        error = "the device has no buffer device addresses";
        return false;
    }
    if (const char* limit = AbxSetting("d3d12_x.indirect_groups_max")) {
        char* end = nullptr;
        const unsigned long long value = std::strtoull(limit, &end, 10);
        if (end == limit || *end != '\0' || value == 0 || value > kDefaultMaxGroups) {
            AbxUnsupported("d3d12_x.indirect_groups_max", "a value that is not a whole number from 1 to 4194304");
        }
        maxGroups_ = static_cast<std::uint32_t>(value);
    }
    const std::uint32_t* limits = device_.Properties().limits.maxComputeWorkGroupCount;
    for (int axis = 0; axis < 3; ++axis) maxDimension_[axis] = (std::min)(limits[axis], maxGroups_);
    meshMaxGroups_ = (std::min)(device_.MeshMaxTotal(), maxGroups_);
    for (int axis = 0; axis < 3; ++axis) meshMaxDimension_[axis] = (std::min)(device_.MeshMaxGroups()[axis], meshMaxGroups_);
    if (!CreateHostBuffer(device_, kScratchBytes,
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, scratch_,
                          error)) {
        return false;
    }
    if (scratch_.address == 0 || scratch_.mapped == nullptr) {
        error = "the scratch buffer has no device address or is not mapped";
        return false;
    }
    std::memset(scratch_.mapped, 0, static_cast<std::size_t>(kHeaderBytes));
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = sizeof(kClampSpirv);
    moduleInfo.pCode = kClampSpirv;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vk.createShaderModule(device_.Handle(), &moduleInfo, nullptr, &module) != VK_SUCCESS) {
        error = "vkCreateShaderModule failed for the argument check";
        return false;
    }
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ClampPush)};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vk.createPipelineLayout(device_.Handle(), &layoutInfo, nullptr, &layout_) != VK_SUCCESS) {
        vk.destroyShaderModule(device_.Handle(), module, nullptr);
        error = "vkCreatePipelineLayout failed for the argument check";
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
        error = "vkCreateComputePipelines failed for the argument check with " + std::to_string(static_cast<int>(result));
        return false;
    }
    return true;
}

std::string IndirectClamp::RecentReport() {
    IndirectClamp* clamp = nullptr;
    {
        const std::lock_guard guard(g_clampLock);
        clamp = g_clamp;
    }
    if (clamp == nullptr) return "";
    const std::lock_guard guard(clamp->recentLock_);
    if (clamp->recentCount_ == 0) return "; no indirect arguments were checked";
    std::uint32_t stats[4];
    std::memcpy(stats, clamp->scratch_.mapped, sizeof(stats));
    char text[512];
    std::snprintf(text, sizeof(text), "; indirect argument check: %u record(s) emptied so far (last %u x %u x %u), %llu batch(es) checked; latest batches as the GPU left them:", stats[0],
                  stats[1], stats[2], stats[3], static_cast<unsigned long long>(clamp->recentCount_));
    std::string report = text;
    const std::size_t shown = static_cast<std::size_t>((std::min)(clamp->recentCount_, static_cast<std::uint64_t>(kRecent)));
    for (std::size_t back = 0; back < shown; ++back) {
        const Recent& recent = clamp->recent_[(clamp->recentCount_ - 1 - back) % kRecent];
        const auto* words = reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(clamp->scratch_.mapped) + recent.at);
        std::uint64_t groups = 0;
        std::uint64_t largest = 0;
        for (std::uint32_t record = 0; record < recent.records; ++record) {
            const std::uint32_t* xyz = words + static_cast<std::size_t>(record) * recent.strideWords;
            const std::uint64_t product = static_cast<std::uint64_t>(xyz[0]) * xyz[1] * xyz[2];
            groups += product;
            largest = (std::max)(largest, product);
        }
        std::snprintf(text, sizeof(text), " [%s, %u record(s), %llu groups in all, largest %llu:", recent.mesh ? "mesh" : "compute", recent.records,
                      static_cast<unsigned long long>(groups), static_cast<unsigned long long>(largest));
        report += text;
        for (std::uint32_t record = 0; record < recent.records && record < 4; ++record) {
            const std::uint32_t* xyz = words + static_cast<std::size_t>(record) * recent.strideWords;
            std::snprintf(text, sizeof(text), " %u x %u x %u", xyz[0], xyz[1], xyz[2]);
            report += text;
        }
        report += "]";
    }
    const std::size_t meshShown = static_cast<std::size_t>((std::min)(clamp->recentMeshCount_, static_cast<std::uint64_t>(kRecentMesh)));
    std::uint64_t meshGroups = 0;
    std::uint64_t meshLargest[3] = {};
    std::uint32_t meshLargestSize[3][3] = {};
    std::string latest;
    for (std::size_t back = 0; back < meshShown; ++back) {
        const Recent& recent = clamp->recentMesh_[(clamp->recentMeshCount_ - 1 - back) % kRecentMesh];
        const auto* words = reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(clamp->scratch_.mapped) + recent.at);
        std::uint64_t groups = 0;
        for (std::uint32_t record = 0; record < recent.records; ++record) {
            const std::uint32_t* xyz = words + static_cast<std::size_t>(record) * recent.strideWords;
            const std::uint64_t product = static_cast<std::uint64_t>(xyz[0]) * xyz[1] * xyz[2];
            groups += product;
            for (std::size_t rank = 0; rank < 3; ++rank) {
                if (product <= meshLargest[rank]) continue;
                for (std::size_t lower = 2; lower > rank; --lower) {
                    meshLargest[lower] = meshLargest[lower - 1];
                    std::memcpy(meshLargestSize[lower], meshLargestSize[lower - 1], sizeof(meshLargestSize[lower]));
                }
                meshLargest[rank] = product;
                std::memcpy(meshLargestSize[rank], xyz, sizeof(meshLargestSize[rank]));
                break;
            }
        }
        meshGroups += groups;
        if (back < 6) {
            std::snprintf(text, sizeof(text), " %u:%llu", recent.records, static_cast<unsigned long long>(groups));
            latest += text;
        }
    }
    std::snprintf(text, sizeof(text),
                  "; the latest %zu indirect mesh batches as the GPU left them (%llu in all): %llu groups together, the three largest records %u x %u x %u, %u x %u x %u and "
                  "%u x %u x %u; newest first as records:groups",
                  meshShown, static_cast<unsigned long long>(clamp->recentMeshCount_), static_cast<unsigned long long>(meshGroups), meshLargestSize[0][0], meshLargestSize[0][1],
                  meshLargestSize[0][2], meshLargestSize[1][0], meshLargestSize[1][1], meshLargestSize[1][2], meshLargestSize[2][0], meshLargestSize[2][1], meshLargestSize[2][2]);
    report += text;
    report += latest;
    return report;
}

VkDeviceSize IndirectClamp::Reserve(VkDeviceSize room) {
    const VkDeviceSize usable = kScratchBytes - kHeaderBytes;
    for (;;) {
        std::uint64_t current = next_.load();
        const std::uint64_t start = (current % usable) + room > usable ? current + (usable - current % usable) : current;
        if (next_.compare_exchange_weak(current, start + room)) return kHeaderBytes + start % usable;
    }
}

void IndirectClamp::Trace(VkCommandBuffer commands, const ClampedArguments& checked, std::uint32_t records, std::uint32_t stride, bool mesh, VkBuffer count,
                          VkDeviceSize countOffset, std::uint64_t titleAddress, std::string label) {
    Traced traced;
    traced.at = checked.offset;
    traced.records = records;
    traced.strideWords = stride / 4;
    traced.titleAddress = titleAddress;
    traced.mesh = mesh;
    traced.label = std::move(label);
    auto* bytes = static_cast<std::uint8_t*>(scratch_.mapped);
    for (std::uint32_t record = 0; record < records; ++record) {
        const std::uint32_t words[3] = {kNotRun, kNotRun, kNotRun};
        std::memcpy(bytes + traced.at + static_cast<std::size_t>(record) * stride, words, sizeof(words));
    }
    if (count != VK_NULL_HANDLE) {
        traced.counted = true;
        traced.countAt = Reserve(16);
        std::memcpy(bytes + traced.countAt, &kNotRun, sizeof(kNotRun));
        Barrier(device_, commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkBufferCopy region{countOffset, traced.countAt, 4};
        device_.Vk().cmdCopyBuffer(commands, count, scratch_.buffer, 1, &region);
        Barrier(device_, commands, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    }
    const std::lock_guard guard(tracedLock_);
    traced_.push_back(std::move(traced));
}

void IndirectClamp::ReportTraced(std::uint64_t present) {
    IndirectClamp* clamp = nullptr;
    {
        const std::lock_guard guard(g_clampLock);
        clamp = g_clamp;
    }
    if (clamp == nullptr) {
        AbxLog("d3d12_x", "TRACE indirect arguments at present %llu: none were checked, so none can be shown (d3d12_x.clamp_indirect_dispatches = 0, or no indirect work yet)",
               static_cast<unsigned long long>(present));
        return;
    }
    std::vector<Traced> ran;
    std::uint64_t waiting = 0;
    std::uint64_t never = 0;
    const auto* bytes = static_cast<const std::uint8_t*>(clamp->scratch_.mapped);
    {
        const std::lock_guard guard(clamp->tracedLock_);
        std::vector<Traced> kept;
        for (Traced& traced : clamp->traced_) {
            std::uint32_t first = 0;
            std::memcpy(&first, bytes + traced.at, sizeof(first));
            if (first != kNotRun) {
                if (!traced.waited || present <= traced.waitedAt + 1) ran.push_back(std::move(traced));
            } else if (!traced.waited) {
                traced.waited = true;
                traced.waitedAt = present;
                ++waiting;
                kept.push_back(std::move(traced));
            } else {
                ++never;
                AbxLog("d3d12_x", "TRACE indirect %s%s: the GPU has not run it two traced presents after it was recorded", traced.mesh ? "mesh" : "compute", traced.label.c_str());
            }
        }
        clamp->traced_.swap(kept);
    }
    std::uint64_t batches[2] = {};
    std::uint64_t empty[2] = {};
    std::uint64_t groupsInAll[2] = {};
    for (const Traced& traced : ran) {
        std::uint32_t counted = traced.records;
        char countText[64] = {};
        if (traced.counted) {
            std::uint32_t value = 0;
            std::memcpy(&value, bytes + traced.countAt, sizeof(value));
            if (value == kNotRun) {
                std::snprintf(countText, sizeof(countText), ", count not copied yet");
            } else {
                std::snprintf(countText, sizeof(countText), ", count %u", value);
                counted = (std::min)(counted, value);
            }
        }
        std::uint64_t groups = 0;
        std::string shown;
        for (std::uint32_t record = 0; record < counted; ++record) {
            std::uint32_t xyz[3] = {};
            std::memcpy(xyz, bytes + traced.at + static_cast<std::size_t>(record) * traced.strideWords * 4, sizeof(xyz));
            groups += static_cast<std::uint64_t>(xyz[0]) * xyz[1] * xyz[2];
            if (record < 4) {
                char text[64];
                std::snprintf(text, sizeof(text), " %u x %u x %u", xyz[0], xyz[1], xyz[2]);
                shown += text;
            }
        }
        const int kind = traced.mesh ? 0 : 1;
        ++batches[kind];
        if (groups == 0) ++empty[kind];
        groupsInAll[kind] += groups;
        AbxLog("d3d12_x", "TRACE indirect %s%s: up to %u record(s) of %u bytes at title 0x%llx%s, %llu groups in all:%s", traced.mesh ? "mesh" : "compute", traced.label.c_str(),
               traced.records, traced.strideWords * 4, static_cast<unsigned long long>(traced.titleAddress), countText, static_cast<unsigned long long>(groups), shown.c_str());
    }
    AbxLog("d3d12_x", "TRACE indirect arguments at present %llu, as the GPU read them: %llu mesh batch(es), %llu of them with no groups, %llu groups in all; %llu compute batch(es), "
                      "%llu of them with no groups, %llu groups in all; %llu recorded but not run yet (shown at the next traced present), %llu never run",
           static_cast<unsigned long long>(present), static_cast<unsigned long long>(batches[0]), static_cast<unsigned long long>(empty[0]),
           static_cast<unsigned long long>(groupsInAll[0]), static_cast<unsigned long long>(batches[1]), static_cast<unsigned long long>(empty[1]),
           static_cast<unsigned long long>(groupsInAll[1]), static_cast<unsigned long long>(waiting), static_cast<unsigned long long>(never));
}

void IndirectClamp::Report() {
    std::uint32_t stats[4];
    std::memcpy(stats, scratch_.mapped, sizeof(stats));
    const std::uint32_t before = reported_.load();
    if (stats[0] == before || (stats[0] > 8 && stats[0] / 2 < before)) return;
    reported_.store(stats[0]);
    AbxLog("d3d12_x", "the GPU found %u indirect dispatch record(s) with impossible group counts so far and ran them empty (last: %u x %u x %u): work that should have written them was "
                      "skipped or wrong",
           stats[0], stats[1], stats[2], stats[3]);
}

std::optional<ClampedArguments> IndirectClamp::RecordDispatches(VkCommandBuffer commands, VkBuffer arguments, VkDeviceSize offset, std::uint32_t records, std::uint32_t stride,
                                                              bool mesh, std::string& error) {
    if (records == 0 || stride < 12 || stride % 4 != 0) {
        error = "indirect dispatch records with a stride that is not a multiple of 4 or below 12 bytes";
        return std::nullopt;
    }
    const VkDeviceSize span = static_cast<VkDeviceSize>(stride) * (records - 1) + 12;
    const VkDeviceSize room = (span + 15) & ~VkDeviceSize{15};
    const VkDeviceSize usable = kScratchBytes - kHeaderBytes;
    if (room > usable / 4) {
        error = "indirect dispatch records of " + std::to_string(span) + " bytes are more than the argument check keeps";
        return std::nullopt;
    }
    Report();
    const VkDeviceSize at = Reserve(room);
    const DeviceFunctions& vk = device_.Vk();
    Barrier(device_, commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkBufferCopy region{offset, at, span};
    vk.cmdCopyBuffer(commands, arguments, scratch_.buffer, 1, &region);
    Barrier(device_, commands, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    {
        const std::lock_guard guard(recentLock_);
        recent_[recentCount_ % kRecent] = Recent{at, records, stride / 4, mesh};
        ++recentCount_;
        if (mesh) {
            recentMesh_[recentMeshCount_ % kRecentMesh] = Recent{at, records, stride / 4, mesh};
            ++recentMeshCount_;
        }
    }
    ClampPush push{};
    push.arguments = scratch_.address + at;
    push.stats = scratch_.address;
    push.records = records;
    push.strideWords = stride / 4;
    const std::uint32_t* dimensions = mesh ? meshMaxDimension_ : maxDimension_;
    push.maxX = dimensions[0];
    push.maxY = dimensions[1];
    push.maxZ = dimensions[2];
    push.maxGroups = mesh ? meshMaxGroups_ : maxGroups_;
    vk.cmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vk.cmdPushConstants(commands, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vk.cmdDispatch(commands, 1, 1, 1);
    Barrier(device_, commands, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    return ClampedArguments{scratch_.buffer, at, span};
}

}
