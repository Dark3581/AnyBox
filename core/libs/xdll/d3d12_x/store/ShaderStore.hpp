#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace abx::d3d12x {

using Sha256 = std::array<std::uint8_t, 32>;

std::optional<Sha256> Sha256Of(const void* data, std::size_t size);
std::string Hex(const Sha256& digest);
std::optional<std::size_t> ContainerSize(const void* data, std::size_t length);

struct ShaderModule {
    std::string key;
    std::string stage;
    std::uint32_t subgroupSize = 0;
    std::wstring path;
};

class ShaderStore {
public:
    bool Load(const std::wstring& folder, std::string& error);
    const ShaderModule* Find(const void* bytecode, std::size_t length) const;
    std::size_t Size() const { return modules_.size(); }

private:
    std::unordered_map<std::string, ShaderModule> modules_;
};

const ShaderStore& TitleShaderStore();

}
