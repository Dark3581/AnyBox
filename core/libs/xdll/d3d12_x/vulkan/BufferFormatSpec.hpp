#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace abx::d3d12x {

struct BufferFormatSource {
    std::uint32_t wordSpecId = 0;
    std::uint32_t knownSpecId = 0;
    bool lowFromUser = false;
    std::uint32_t low = 0;
    bool highFromUser = false;
    std::uint32_t high = 0;
    std::int64_t offset = 0;
};

std::vector<BufferFormatSource> ReadBufferFormatSources(const std::uint32_t* words, std::size_t count, std::string& error);

std::optional<std::uint64_t> BufferFormatAddress(const BufferFormatSource& source, const std::uint32_t* userSgprs, std::size_t userSgprCount);

std::vector<std::pair<std::uint32_t, std::uint32_t>> BufferFormatConstants(const std::vector<BufferFormatSource>& sources, const std::vector<std::uint32_t>& values);

}
