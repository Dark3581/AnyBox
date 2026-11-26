#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace abx::d3d12x {

struct LaneTurns {
    std::uint32_t turns = 0;
    std::size_t regions = 0;
    std::size_t variables = 0;
    std::size_t carried = 0;
};

std::optional<LaneTurns> RunLaneGroupInTurns(std::vector<std::uint32_t>& words, std::uint32_t lanes, std::string& error);

}
