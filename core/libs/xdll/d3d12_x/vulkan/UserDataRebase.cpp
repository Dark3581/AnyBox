#include "UserDataRebase.hpp"

#include <map>
#include <set>
#include <utility>

namespace abx::d3d12x {

namespace {

constexpr std::uint32_t kMagic = 0x07230203;
constexpr std::uint32_t kHeaderWords = 5;
constexpr std::uint32_t kOpConstant = 43;
constexpr std::uint32_t kOpVariable = 59;
constexpr std::uint32_t kOpAccessChain = 65;
constexpr std::uint32_t kOpInBoundsAccessChain = 66;
constexpr std::uint32_t kStoragePushConstant = 9;

struct Constant {
    std::uint32_t type = 0;
    std::uint32_t value = 0;
};

}

std::optional<UserDataRebase> RebaseUserData(std::vector<std::uint32_t>& words, std::uint32_t reservedFrom, std::uint32_t reservedTo, std::uint32_t registers, std::string& error) {
    if (words.size() < kHeaderWords || words[0] != kMagic) {
        error = "not a SPIR-V module";
        return std::nullopt;
    }
    if (reservedTo > reservedFrom || reservedFrom > registers) {
        error = "the user data registers can only move down";
        return std::nullopt;
    }
    const std::uint32_t shift = reservedFrom - reservedTo;
    std::map<std::uint32_t, Constant> constants;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> byValue;
    std::set<std::uint32_t> blocks;
    std::set<std::uint32_t> registerArrays;
    for (std::size_t at = kHeaderWords; at < words.size();) {
        const std::uint32_t count = words[at] >> 16;
        const std::uint32_t op = words[at] & 0xFFFFu;
        if (count == 0 || at + count > words.size()) {
            error = "a malformed instruction";
            return std::nullopt;
        }
        if (op == kOpConstant && count == 4) {
            constants[words[at + 2]] = {words[at + 1], words[at + 3]};
            byValue.emplace(std::make_pair(words[at + 1], words[at + 3]), words[at + 2]);
        } else if (op == kOpVariable && count >= 4 && words[at + 3] == kStoragePushConstant) {
            blocks.insert(words[at + 2]);
        }
        at += count;
    }
    UserDataRebase result;
    if (blocks.empty() || shift == 0) return result;
    const auto constantAt = [&](std::uint32_t id) -> const Constant* {
        const auto found = constants.find(id);
        return found != constants.end() ? &found->second : nullptr;
    };
    for (std::size_t at = kHeaderWords; at < words.size();) {
        const std::uint32_t count = words[at] >> 16;
        const std::uint32_t op = words[at] & 0xFFFFu;
        if ((op == kOpAccessChain || op == kOpInBoundsAccessChain) && count >= 5) {
            const std::uint32_t base = words[at + 3];
            std::size_t registerIndex = 0;
            if (blocks.count(base) != 0) {
                const Constant* member = constantAt(words[at + 4]);
                if (member == nullptr) {
                    error = "a push constant member chosen by a value that is not a constant";
                    return std::nullopt;
                }
                if (member->value == 0) {
                    if (count == 5) registerArrays.insert(words[at + 2]);
                    else registerIndex = at + 5;
                }
            } else if (registerArrays.count(base) != 0) {
                registerIndex = at + 4;
            }
            if (registerIndex != 0) {
                const Constant* index = constantAt(words[registerIndex]);
                if (index == nullptr) {
                    error = "a user data register chosen by a value that is not a constant";
                    return std::nullopt;
                }
                if (index->value >= registers) {
                    error = "a user data register past the " + std::to_string(registers) + " the block holds";
                    return std::nullopt;
                }
                if (index->value >= reservedTo) {
                    const std::uint32_t wanted = index->value >= reservedFrom ? index->value - shift : index->value - reservedTo;
                    const auto replacement = byValue.find({index->type, wanted});
                    if (replacement == byValue.end()) {
                        error = "the module has no constant " + std::to_string(wanted) + " to move register " + std::to_string(index->value) + " to";
                        return std::nullopt;
                    }
                    words[registerIndex] = replacement->second;
                    ++result.rewritten;
                } else {
                    ++result.untouched;
                }
            }
        }
        at += count;
    }
    return result;
}

}
