#pragma once

#include <cstdint>

namespace abx::xmem {

bool IsGpuVisible(std::uint32_t protect);
void AnnounceAvailable(std::uint64_t start, std::uint64_t bytes);
void AnnounceWithdrawn(std::uint64_t start, std::uint64_t bytes);
void AnnounceBacking(std::uint64_t view, std::uint64_t bytes);
void AnnounceBackingWithdrawn(std::uint64_t view, std::uint64_t bytes);
void AnnounceAliased(std::uint64_t start, std::uint64_t bytes, std::uint64_t source);

}
