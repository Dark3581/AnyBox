#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace abx::d3d12x {

std::vector<std::string> DescribeThreadStacks(std::uint32_t sampleMilliseconds, std::size_t lineChars);
std::string DescribeCode(std::uint64_t address);

}
