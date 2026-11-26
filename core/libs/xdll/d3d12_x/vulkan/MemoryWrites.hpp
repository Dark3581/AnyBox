#pragma once

#include <cstdint>
#include <vector>

namespace abx::d3d12x {

bool MayWriteMemory(const std::vector<std::uint32_t>& words);

}
