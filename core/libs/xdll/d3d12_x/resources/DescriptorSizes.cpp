#include "DescriptorSizes.hpp"

#include "AbxApi.hpp"

#include <cerrno>
#include <cstdlib>

namespace abx::d3d12x {

bool ParseDescriptorSize(const char* key, const char* text, std::optional<UINT>& size, std::string& error) {
    size.reset();
    if (text == nullptr) {
        return true;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(text, &end, 0);
    if (end == text || *end != '\0' || errno != 0 || value == 0 || value > 0xFFFFu || text[0] == '-') {
        error = std::string("setting ") + key + " = " + text + " is not a descriptor size in bytes";
        return false;
    }
    size = static_cast<UINT>(value);
    return true;
}

std::optional<DescriptorSizes> DescriptorSizesFromSettings(std::string& error) {
    DescriptorSizes sizes;
    const struct {
        const char* key;
        std::optional<UINT>* size;
    } fields[] = {{"d3d12_x.descriptor_size.shader_resource", &sizes.shaderResource},
                  {"d3d12_x.descriptor_size.sampler", &sizes.sampler},
                  {"d3d12_x.descriptor_size.render_target", &sizes.renderTarget},
                  {"d3d12_x.descriptor_size.depth_stencil", &sizes.depthStencil}};
    for (const auto& field : fields) {
        if (!ParseDescriptorSize(field.key, AbxSetting(field.key), *field.size, error)) {
            return std::nullopt;
        }
    }
    return sizes;
}

std::optional<UINT> DescriptorIncrement(const DescriptorSizes& sizes, D3D12_DESCRIPTOR_HEAP_TYPE type, std::string& error) {
    const char* name = nullptr;
    std::optional<UINT> size;
    UINT minimum = 1;
    switch (type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
        name = "shader resource";
        size = sizes.shaderResource;
        minimum = kImageDescriptorBytes;
        break;
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
        name = "sampler";
        size = sizes.sampler;
        minimum = kSamplerDescriptorBytes;
        break;
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
        name = "render target";
        size = sizes.renderTarget;
        break;
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
        name = "depth-stencil";
        size = sizes.depthStencil;
        break;
    default:
        error = "descriptor heap type " + std::to_string(static_cast<int>(type)) + " is unknown";
        return std::nullopt;
    }
    if (!size) {
        error = std::string("the ") + name + " descriptor size is unknown: it is the console's increment, which translated shaders compile in, and no title has given it yet";
        return std::nullopt;
    }
    if (*size < minimum || *size % 4 != 0) {
        error = std::string("a ") + name + " descriptor size of " + std::to_string(*size) + " bytes is not a multiple of 4 of at least " + std::to_string(minimum);
        return std::nullopt;
    }
    return size;
}

std::optional<std::uint64_t> DescriptorHeapBytes(UINT count, UINT increment, std::string& error) {
    if (count == 0) {
        error = "a descriptor heap needs at least one descriptor";
        return std::nullopt;
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(count) * increment;
    if (bytes > (1ull << 32)) {
        error = std::to_string(count) + " descriptors of " + std::to_string(increment) + " bytes exceed 4 GB";
        return std::nullopt;
    }
    return bytes;
}

}
