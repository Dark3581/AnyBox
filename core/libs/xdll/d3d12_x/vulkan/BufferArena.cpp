#include "BufferArena.hpp"

#include <iterator>

namespace abx::d3d12x {

BufferArena::BufferArena(std::uint64_t base, std::uint64_t size) : base_(base), size_(size) {
    if (size_ != 0) {
        free_.emplace(0, size_);
    }
}

std::optional<std::uint64_t> BufferArena::Allocate(std::uint64_t bytes, std::uint64_t alignment) {
    if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> guard(lock_);
    for (auto range = free_.begin(); range != free_.end(); ++range) {
        const std::uint64_t start = range->first;
        const std::uint64_t length = range->second;
        const std::uint64_t address = base_ + start;
        const std::uint64_t aligned = (address + alignment - 1) & ~(alignment - 1);
        const std::uint64_t padding = aligned - address;
        if (padding > length || bytes > length - padding) {
            continue;
        }
        const std::uint64_t offset = start + padding;
        const std::uint64_t tail = length - padding - bytes;
        free_.erase(range);
        if (padding != 0) {
            free_.emplace(start, padding);
        }
        if (tail != 0) {
            free_.emplace(offset + bytes, tail);
        }
        used_.emplace(offset, bytes);
        return offset;
    }
    return std::nullopt;
}

bool BufferArena::Free(std::uint64_t offset) {
    std::lock_guard<std::mutex> guard(lock_);
    const auto used = used_.find(offset);
    if (used == used_.end()) {
        return false;
    }
    std::uint64_t start = offset;
    std::uint64_t length = used->second;
    used_.erase(used);
    auto next = free_.lower_bound(start);
    if (next != free_.end() && next->first == start + length) {
        length += next->second;
        next = free_.erase(next);
    }
    if (next != free_.begin()) {
        auto previous = std::prev(next);
        if (previous->first + previous->second == start) {
            start = previous->first;
            length += previous->second;
            free_.erase(previous);
        }
    }
    free_.emplace(start, length);
    return true;
}

std::uint64_t BufferArena::Used() const {
    std::lock_guard<std::mutex> guard(lock_);
    std::uint64_t total = 0;
    for (const auto& [offset, bytes] : used_) {
        total += bytes;
    }
    return total;
}

std::size_t BufferArena::FreeRanges() const {
    std::lock_guard<std::mutex> guard(lock_);
    return free_.size();
}

}
