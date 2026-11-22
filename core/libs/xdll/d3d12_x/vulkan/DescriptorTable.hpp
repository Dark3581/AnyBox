#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace abx::d3d12x {

using DescriptorKey = std::array<std::uint32_t, 8>;

constexpr std::uint32_t kDescriptorProbes = 8;
constexpr std::uint32_t kDescriptorEntryWords = 10;

DescriptorKey ImageKey(const std::uint32_t (&descriptor)[8]);
DescriptorKey SamplerKey(const std::uint32_t (&descriptor)[4]);
std::uint32_t DescriptorHash(const DescriptorKey& key);

class DescriptorHashTable {
public:
    explicit DescriptorHashTable(std::uint32_t slots = 1024);

    bool Insert(const DescriptorKey& key, std::uint32_t index);
    bool Erase(const DescriptorKey& key);
    std::optional<std::uint32_t> Find(const DescriptorKey& key) const;
    std::uint32_t Lookup(const DescriptorKey& key) const { return Find(key).value_or(0); }

    std::uint32_t Mask() const { return mask_; }
    std::size_t Size() const { return size_; }
    std::uint64_t Generation() const { return generation_; }
    const std::vector<std::uint32_t>& Words() const { return words_; }

private:
    std::uint32_t* Slot(std::uint32_t slot) { return words_.data() + 1 + static_cast<std::size_t>(slot) * kDescriptorEntryWords; }
    const std::uint32_t* Slot(std::uint32_t slot) const { return words_.data() + 1 + static_cast<std::size_t>(slot) * kDescriptorEntryWords; }
    bool Place(const DescriptorKey& key, std::uint32_t index);
    void Grow();

    std::uint32_t mask_ = 0;
    std::size_t size_ = 0;
    std::uint64_t generation_ = 0;
    std::vector<std::uint32_t> words_;
};

}
