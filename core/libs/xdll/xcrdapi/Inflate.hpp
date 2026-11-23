#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace abx::xcrdapi {

bool InflateZlib(const std::uint8_t* source, std::size_t sourceBytes, std::uint8_t* destination, std::size_t destinationBytes, std::size_t& written, std::string& error);

}
