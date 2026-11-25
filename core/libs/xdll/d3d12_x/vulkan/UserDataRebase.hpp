#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace abx::d3d12x {

struct UserDataRebase {
    std::size_t rewritten = 0;
    std::size_t untouched = 0;
};

std::optional<UserDataRebase> RebaseUserData(std::vector<std::uint32_t>& words, std::uint32_t reservedFrom, std::uint32_t reservedTo, std::uint32_t registers, std::string& error);

}
