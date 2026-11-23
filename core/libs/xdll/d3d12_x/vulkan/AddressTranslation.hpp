#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace abx::d3d12x {

constexpr std::uint32_t kHeapBaseLowMember = 1;
constexpr std::uint32_t kHeapBaseHighMember = 2;
constexpr std::uint32_t kHeapBaseLowOffset = 128;
constexpr std::uint32_t kHeapBaseHighOffset = 132;
constexpr std::uint32_t kCodeBaseLowMember = 3;
constexpr std::uint32_t kCodeBaseHighMember = 4;

struct AddressTranslation {
    std::vector<std::uint32_t> words;
    std::size_t rewritten = 0;
    bool usesCodeBase = false;
};

std::optional<AddressTranslation> TranslateTitleAddresses(std::span<const std::uint32_t> words, std::string& error);

}
