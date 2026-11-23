#pragma once

#include <cstdint>

namespace abx::xmem {

bool IsGpuVisible(std::uint32_t protect);
void AnnounceAvailable(std::uint64_t start, std::uint64_t bytes);
void AnnounceWithdrawn(std::uint64_t start, std::uint64_t bytes);

}
