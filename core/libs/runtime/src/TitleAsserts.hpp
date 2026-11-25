#pragma once

#include <cstddef>
#include <cstdint>

namespace abx {

bool IsSkippableAssertBreak(const std::uint8_t* breakpoint, std::size_t bytesBefore);

void InstallTitleAssertPolicy();

}
