#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>

namespace abx::d3d12x {

class BufferArena {
public:
    BufferArena(std::uint64_t base, std::uint64_t size);
    BufferArena(const BufferArena&) = delete;
    BufferArena& operator=(const BufferArena&) = delete;

    std::optional<std::uint64_t> Allocate(std::uint64_t bytes, std::uint64_t alignment);
    bool Free(std::uint64_t offset);
    std::uint64_t Used() const;
    std::uint64_t Size() const { return size_; }
    std::size_t FreeRanges() const;

private:
    std::uint64_t base_;
    std::uint64_t size_;
    mutable std::mutex lock_;
    std::map<std::uint64_t, std::uint64_t> free_;
    std::map<std::uint64_t, std::uint64_t> used_;
};

}
