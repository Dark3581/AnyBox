#pragma once

#include "Resource.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace abx::d3d12x {

class TitleTextures {
public:
    TitleTextures(const ResourceContext& context, IUnknown* parent) : context_(context), parent_(parent) {}
    ~TitleTextures();
    TitleTextures(const TitleTextures&) = delete;
    TitleTextures& operator=(const TitleTextures&) = delete;

    Resource* At(std::uint64_t address, const D3D12_RESOURCE_DESC& desc, std::string& error);

private:
    enum class Filled { Yes, NotMapped, Failed };
    Filled Upload(Resource& texture, std::uint64_t address, std::uint32_t mips, std::string& error);
    bool MakeReadable(Resource& texture, std::string& error);

    ResourceContext context_;
    IUnknown* parent_;
    std::mutex lock_;
    std::map<std::pair<std::uint64_t, std::vector<std::uint8_t>>, Resource*> textures_;
    std::map<Resource*, std::pair<std::uint64_t, std::uint32_t>> pending_;
};

}
