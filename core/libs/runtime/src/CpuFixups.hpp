#pragma once

#include <cstddef>
#include <cstdint>

namespace abx {

std::size_t CacheLineWritebackLength(const std::uint8_t* code, std::size_t available);
void InstallCpuFixups();

}
