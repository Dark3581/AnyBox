#include "DescriptorTable.hpp"

#include <algorithm>

namespace abx::d3d12x {

namespace {

constexpr std::uint32_t kFnvOffset = 2166136261u;
constexpr std::uint32_t kFnvPrime = 16777619u;
constexpr std::uint32_t kMaxSlots = 1u << 24;

bool IsEmptyKey(const DescriptorKey& key) {
    return std::all_of(key.begin(), key.end(), [](std::uint32_t word) { return word == 0; });
}

bool Matches(const std::uint32_t* slot, const DescriptorKey& key) {
    return std::equal(key.begin(), key.end(), slot);
}

std::uint32_t RoundUpPowerOfTwo(std::uint32_t value) {
    std::uint32_t result = 1;
    while (result < value && result < kMaxSlots) {
        result <<= 1;
    }
    return result;
}

}

DescriptorKey ImageKey(const std::uint32_t (&descriptor)[8]) {
    DescriptorKey key{};
    std::copy(descriptor, descriptor + 8, key.begin());
    return key;
}

DescriptorKey SamplerKey(const std::uint32_t (&descriptor)[4]) {
    DescriptorKey key{};
    std::copy(descriptor, descriptor + 4, key.begin());
    std::fill(key.begin() + 4, key.end(), 0xFFFFFFFFu);
    return key;
}

std::uint32_t DescriptorHash(const DescriptorKey& key) {
    std::uint32_t hash = kFnvOffset;
    for (const std::uint32_t word : key) {
        hash = (hash ^ word) * kFnvPrime;
    }
    return hash;
}

DescriptorHashTable::DescriptorHashTable(std::uint32_t slots) {
    const std::uint32_t count = RoundUpPowerOfTwo(std::max(slots, kDescriptorProbes));
    mask_ = count - 1;
    words_.assign(1 + static_cast<std::size_t>(count) * kDescriptorEntryWords, 0);
    words_[0] = mask_;
}

bool DescriptorHashTable::Place(const DescriptorKey& key, std::uint32_t index) {
    const std::uint32_t hash = DescriptorHash(key);
    std::uint32_t* free = nullptr;
    for (std::uint32_t probe = 0; probe < kDescriptorProbes; ++probe) {
        std::uint32_t* slot = Slot((hash + probe) & mask_);
        if (Matches(slot, key)) {
            slot[8] = index;
            return true;
        }
        if (free == nullptr && slot[8] == 0) {
            free = slot;
        }
    }
    if (free == nullptr) {
        return false;
    }
    std::copy(key.begin(), key.end(), free);
    free[8] = index;
    ++size_;
    return true;
}

void DescriptorHashTable::Grow() {
    std::vector<std::pair<DescriptorKey, std::uint32_t>> entries;
    for (std::uint32_t slot = 0; slot <= mask_; ++slot) {
        const std::uint32_t* entry = Slot(slot);
        if (entry[8] != 0) {
            DescriptorKey key{};
            std::copy(entry, entry + 8, key.begin());
            entries.emplace_back(key, entry[8]);
        }
    }
    std::uint32_t count = mask_ + 1;
    for (;;) {
        count <<= 1;
        mask_ = count - 1;
        words_.assign(1 + static_cast<std::size_t>(count) * kDescriptorEntryWords, 0);
        words_[0] = mask_;
        size_ = 0;
        bool placed = true;
        for (const auto& [key, index] : entries) {
            if (!Place(key, index)) {
                placed = false;
                break;
            }
        }
        if (placed || count >= kMaxSlots) {
            return;
        }
    }
}

bool DescriptorHashTable::Insert(const DescriptorKey& key, std::uint32_t index) {
    if (index == 0 || IsEmptyKey(key)) {
        return false;
    }
    ++generation_;
    while (!Place(key, index)) {
        if (mask_ + 1 >= kMaxSlots) {
            return false;
        }
        Grow();
    }
    return true;
}

bool DescriptorHashTable::Erase(const DescriptorKey& key) {
    const std::uint32_t hash = DescriptorHash(key);
    for (std::uint32_t probe = 0; probe < kDescriptorProbes; ++probe) {
        std::uint32_t* slot = Slot((hash + probe) & mask_);
        if (slot[8] != 0 && Matches(slot, key)) {
            std::fill(slot, slot + kDescriptorEntryWords, 0);
            --size_;
            ++generation_;
            return true;
        }
    }
    return false;
}

std::optional<std::uint32_t> DescriptorHashTable::Find(const DescriptorKey& key) const {
    const std::uint32_t hash = DescriptorHash(key);
    for (std::uint32_t probe = 0; probe < kDescriptorProbes; ++probe) {
        const std::uint32_t* slot = Slot((hash + probe) & mask_);
        if (Matches(slot, key)) {
            return slot[8] == 0 ? std::nullopt : std::optional<std::uint32_t>(slot[8]);
        }
    }
    return std::nullopt;
}

}
