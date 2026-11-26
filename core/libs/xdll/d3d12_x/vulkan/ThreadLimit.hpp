#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace abx::d3d12x {

std::optional<std::array<std::uint32_t, 3>> ComputeGroupSize(const std::vector<std::uint32_t>& words, std::string& error);

bool LimitThreads(std::vector<std::uint32_t>& words, std::uint32_t limitRegister, std::string& error);

}
