#include "DescriptorSizes.hpp"

namespace abx::d3d12x {

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
