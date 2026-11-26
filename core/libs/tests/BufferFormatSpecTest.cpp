#include "BufferFormatSpec.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace abx::d3d12x;

int g_failures = 0;

void Expect(bool condition, const std::string& what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

constexpr std::uint32_t kOpName = 5;
constexpr std::uint32_t kOpDecorate = 71;
constexpr std::uint32_t kOpTypeVoid = 19;
constexpr std::uint32_t kSpecId = 1;

void Name(std::vector<std::uint32_t>& words, std::uint32_t id, const std::string& text) {
    std::vector<std::uint32_t> packed((text.size() + 4) / 4, 0);
    for (std::size_t index = 0; index < text.size(); ++index) packed[index / 4] |= static_cast<std::uint32_t>(static_cast<unsigned char>(text[index])) << (8 * (index % 4));
    words.push_back(static_cast<std::uint32_t>((2 + packed.size()) << 16) | kOpName);
    words.push_back(id);
    words.insert(words.end(), packed.begin(), packed.end());
}

void SpecId(std::vector<std::uint32_t>& words, std::uint32_t id, std::uint32_t value) {
    words.push_back((4u << 16) | kOpDecorate);
    words.push_back(id);
    words.push_back(kSpecId);
    words.push_back(value);
}

std::vector<std::uint32_t> Module(const std::vector<std::pair<std::uint32_t, std::string>>& names, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& ids) {
    std::vector<std::uint32_t> words = {0x07230203u, 0x00010500u, 0, 100, 0};
    for (const auto& [id, text] : names) Name(words, id, text);
    for (const auto& [id, value] : ids) SpecId(words, id, value);
    words.push_back((2u << 16) | kOpTypeVoid);
    words.push_back(99);
    return words;
}

void ReadsSources() {
    const std::vector<std::uint32_t> words = Module({{10, "abx_vsharp_word3 known=22017 lo=u4 hi=c0 offset=1612"}, {11, "abx_vsharp_word3 known=22019 lo=u5 hi=u6 offset=-8"},
                                                     {12, "main"}},
                                                    {{10, 22016}, {11, 22018}, {20, 22017}, {21, 22019}});
    std::string error;
    const std::vector<BufferFormatSource> sources = ReadBufferFormatSources(words.data(), words.size(), error);
    Expect(error.empty(), "two sources read without error: " + error);
    Expect(sources.size() == 2, "two sources");
    if (sources.size() != 2) return;
    Expect(sources[0].wordSpecId == 22016 && sources[0].knownSpecId == 22017, "first source ids");
    Expect(sources[0].lowFromUser && sources[0].low == 4 && !sources[0].highFromUser && sources[0].high == 0 && sources[0].offset == 1612, "first source origin");
    Expect(sources[1].lowFromUser && sources[1].low == 5 && sources[1].highFromUser && sources[1].high == 6 && sources[1].offset == -8, "second source origin");

    std::uint32_t user[32] = {};
    user[4] = 0x12345677u;
    const auto first = BufferFormatAddress(sources[0], user, 32);
    Expect(first && *first == 0x12345674ull + 1612, "address drops the low two bits of the pointer and adds the offset");
    user[5] = 0x1000u;
    user[6] = 0x2u;
    const auto second = BufferFormatAddress(sources[1], user, 32);
    Expect(second && *second == 0x200000FF8ull, "a high word from a user register and a negative offset");
    Expect(!BufferFormatAddress(sources[0], user, 4), "a user register past the block is refused");

    const auto constants = BufferFormatConstants(sources, {0xAAAAu, 0xBBBBu});
    Expect(constants.size() == 4 && constants[0] == std::make_pair(22016u, 0xAAAAu) && constants[1] == std::make_pair(22017u, 1u) &&
               constants[2] == std::make_pair(22018u, 0xBBBBu) && constants[3] == std::make_pair(22019u, 1u),
           "constants: each word and its known flag");
    Expect(BufferFormatConstants(sources, {1u}).empty(), "a word count that does not match gives no constants");
}

void RefusesBadModules() {
    std::string error;
    std::vector<std::uint32_t> none = Module({{12, "main"}}, {});
    Expect(ReadBufferFormatSources(none.data(), none.size(), error).empty() && error.empty(), "a module without sources gives none and no error");
    std::vector<std::uint32_t> noSpec = Module({{10, "abx_vsharp_word3 known=22017 lo=u4 hi=c0 offset=12"}}, {});
    Expect(ReadBufferFormatSources(noSpec.data(), noSpec.size(), error).empty() && !error.empty(), "a source without a SpecId is an error");
    std::vector<std::uint32_t> badField = Module({{10, "abx_vsharp_word3 known=22017 lo=v4 hi=c0 offset=12"}}, {{10, 22016}});
    Expect(ReadBufferFormatSources(badField.data(), badField.size(), error).empty() && !error.empty(), "an unknown register kind is an error");
    std::vector<std::uint32_t> highUser = Module({{10, "abx_vsharp_word3 known=22017 lo=u40 hi=c0 offset=12"}}, {{10, 22016}});
    Expect(ReadBufferFormatSources(highUser.data(), highUser.size(), error).empty() && !error.empty(), "a user register past s31 is an error");
    std::vector<std::uint32_t> shared = Module({{10, "abx_vsharp_word3 known=22016 lo=u4 hi=c0 offset=12"}}, {{10, 22016}});
    Expect(ReadBufferFormatSources(shared.data(), shared.size(), error).empty() && !error.empty(), "a known id equal to the word id is an error");
    std::vector<std::uint32_t> missing = Module({{10, "abx_vsharp_word3 known=22017 lo=u4 offset=12"}}, {{10, 22016}});
    Expect(ReadBufferFormatSources(missing.data(), missing.size(), error).empty() && !error.empty(), "a source without hi is an error");
    std::vector<std::uint32_t> truncated = Module({{10, "abx_vsharp_word3 known=22017 lo=u4 hi=c0 offset=12"}}, {{10, 22016}});
    truncated.resize(7);
    Expect(ReadBufferFormatSources(truncated.data(), truncated.size(), error).empty(), "a module cut inside the name gives no source");
    const std::uint32_t garbage[5] = {1, 2, 3, 4, 5};
    Expect(ReadBufferFormatSources(garbage, 5, error).empty() && !error.empty(), "not SPIR-V is an error");
}

}

int main() {
    ReadsSources();
    RefusesBadModules();
    if (g_failures == 0) std::printf("buffer format specialization: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
