#include "BufferFormatSpec.hpp"

#include <cstdlib>
#include <map>
#include <set>
#include <sstream>

namespace abx::d3d12x {

namespace {

constexpr std::uint32_t kMagic = 0x07230203u;
constexpr std::uint32_t kOpName = 5;
constexpr std::uint32_t kOpDecorate = 71;
constexpr std::uint32_t kDecorationSpecId = 1;
constexpr const char* kPrefix = "abx_vsharp_word3";

bool EndsHeader(std::uint32_t opcode) {
    return (opcode >= 19 && opcode <= 39) || (opcode >= 41 && opcode <= 46) || (opcode >= 48 && opcode <= 52) || opcode == 54 || opcode == 59;
}

std::string LiteralString(const std::uint32_t* words, std::size_t count) {
    std::string text;
    for (std::size_t index = 0; index < count; ++index) {
        for (int byte = 0; byte < 4; ++byte) {
            const char c = static_cast<char>((words[index] >> (8 * byte)) & 0xFFu);
            if (c == 0) return text;
            text.push_back(c);
        }
    }
    return text;
}

bool ParseRegister(const std::string& text, bool& fromUser, std::uint32_t& value) {
    if (text.size() < 2 || (text[0] != 'u' && text[0] != 'c')) return false;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str() + 1, &end, 10);
    if (end == nullptr || *end != 0 || parsed > 0xFFFFFFFFull) return false;
    fromUser = text[0] == 'u';
    if (fromUser && parsed >= 32) return false;
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool ParseSource(const std::string& text, BufferFormatSource& source) {
    std::istringstream fields(text);
    std::string field;
    fields >> field;
    if (field != kPrefix) return false;
    bool known = false, low = false, high = false, offset = false;
    while (fields >> field) {
        const std::size_t equals = field.find('=');
        if (equals == std::string::npos) return false;
        const std::string name = field.substr(0, equals);
        const std::string value = field.substr(equals + 1);
        char* end = nullptr;
        if (name == "known") {
            const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
            if (value.empty() || *end != 0 || parsed > 0xFFFFFFFFull) return false;
            source.knownSpecId = static_cast<std::uint32_t>(parsed);
            known = true;
        } else if (name == "lo") {
            if (!ParseRegister(value, source.lowFromUser, source.low)) return false;
            low = true;
        } else if (name == "hi") {
            if (!ParseRegister(value, source.highFromUser, source.high)) return false;
            high = true;
        } else if (name == "offset") {
            const long long parsed = std::strtoll(value.c_str(), &end, 10);
            if (value.empty() || *end != 0) return false;
            source.offset = parsed;
            offset = true;
        } else {
            return false;
        }
    }
    return known && low && high && offset;
}

}

std::vector<BufferFormatSource> ReadBufferFormatSources(const std::uint32_t* words, std::size_t count, std::string& error) {
    error.clear();
    if (words == nullptr || count < 5 || words[0] != kMagic) {
        error = "not a SPIR-V module";
        return {};
    }
    std::map<std::uint32_t, BufferFormatSource> named;
    std::map<std::uint32_t, std::uint32_t> specIds;
    std::size_t at = 5;
    while (at < count) {
        const std::uint32_t wordCount = words[at] >> 16;
        const std::uint32_t opcode = words[at] & 0xFFFFu;
        if (wordCount == 0 || EndsHeader(opcode)) break;
        if (at + wordCount > count) break;
        if (opcode == kOpName && wordCount >= 3) {
            const std::string text = LiteralString(words + at + 2, wordCount - 2);
            if (text.rfind(kPrefix, 0) == 0) {
                BufferFormatSource source;
                if (!ParseSource(text, source)) {
                    error = "a buffer format source the module names cannot be read: " + text;
                    return {};
                }
                named[words[at + 1]] = source;
            }
        } else if (opcode == kOpDecorate && wordCount == 4 && words[at + 2] == kDecorationSpecId) {
            specIds[words[at + 1]] = words[at + 3];
        }
        at += wordCount;
    }
    std::vector<BufferFormatSource> sources;
    std::set<std::uint32_t> used;
    for (auto& [id, source] : named) {
        const auto found = specIds.find(id);
        if (found == specIds.end()) {
            error = "a buffer format source has no specialization constant id";
            return {};
        }
        source.wordSpecId = found->second;
        if (source.wordSpecId == source.knownSpecId || !used.insert(source.wordSpecId).second || !used.insert(source.knownSpecId).second) {
            error = "two buffer format sources share a specialization constant id";
            return {};
        }
        sources.push_back(source);
    }
    return sources;
}

std::optional<std::uint64_t> BufferFormatAddress(const BufferFormatSource& source, const std::uint32_t* userSgprs, std::size_t userSgprCount) {
    if ((source.lowFromUser || source.highFromUser) && userSgprs == nullptr) return std::nullopt;
    if ((source.lowFromUser && source.low >= userSgprCount) || (source.highFromUser && source.high >= userSgprCount)) return std::nullopt;
    const std::uint32_t low = source.lowFromUser ? userSgprs[source.low] : source.low;
    const std::uint32_t high = source.highFromUser ? userSgprs[source.high] : source.high;
    const std::uint64_t base = (static_cast<std::uint64_t>(high) << 32) | (low & ~std::uint32_t{3});
    return base + static_cast<std::uint64_t>(source.offset);
}

std::vector<std::pair<std::uint32_t, std::uint32_t>> BufferFormatConstants(const std::vector<BufferFormatSource>& sources, const std::vector<std::uint32_t>& values) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> constants;
    if (values.size() != sources.size()) return constants;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        constants.emplace_back(sources[index].wordSpecId, values[index]);
        constants.emplace_back(sources[index].knownSpecId, 1u);
    }
    return constants;
}

}
