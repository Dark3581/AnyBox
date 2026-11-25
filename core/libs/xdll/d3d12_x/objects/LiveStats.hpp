#pragma once

#include <atomic>
#include <cstdint>

namespace abx::d3d12x {

struct LiveStats {
    std::atomic<std::uint64_t> presents{0};
    std::atomic<std::uint64_t> draws{0};
    std::atomic<std::uint64_t> dispatches{0};
    std::atomic<std::uint64_t> skippedBuilding{0};
    std::atomic<std::uint64_t> skippedOther{0};
    std::atomic<std::uint64_t> pipelinesBuilt{0};
    std::atomic<std::uint64_t> pipelinesFailed{0};
    std::atomic<std::uint64_t> pipelinesQueued{0};
    std::atomic<std::uint64_t> pipelinesBuilding{0};
    std::atomic<std::uint64_t> textureBytes{0};
    std::atomic<std::uint64_t> textureCount{0};
    std::atomic<std::uint64_t> scratchBytes{0};
    std::atomic<std::uint64_t> scratchBuffers{0};
    std::atomic<std::uint64_t> scratchSystemBytes{0};
    std::atomic<std::uint64_t> poolBlockBytes{0};
    std::atomic<std::uint64_t> textureBucketBytes[6] = {};
    std::atomic<std::uint64_t> textureBucketCount[6] = {};
};

std::uint32_t TextureBucket(bool written, std::uint64_t bytes);

LiveStats& Stats();

void StartLiveStats();

bool TracingFrame();

}
