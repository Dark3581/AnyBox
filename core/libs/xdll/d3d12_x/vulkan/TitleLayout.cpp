#include "TitleLayout.hpp"

namespace abx::d3d12x {

namespace {

BindingSpec Spec(std::uint32_t binding, DescriptorKind kind, ImageDim dim, bool arrayed, std::uint32_t count) {
    return BindingSpec{binding, kind, dim, arrayed, count};
}

std::vector<BindingSpec> Build() {
    using K = DescriptorKind;
    using D = ImageDim;
    std::vector<BindingSpec> bindings = {
        Spec(0, K::SampledImage, D::Dim2D, false, 8192),
        Spec(1, K::Sampler, D::None, false, 2048),
        Spec(2, K::StorageBuffer, D::None, false, 1),
        Spec(3, K::SampledImage, D::Dim2D, true, 2048),
        Spec(4, K::StorageImage, D::Dim2D, false, 1024),
        Spec(5, K::SampledImage, D::Dim1D, false, 256),
        Spec(6, K::SampledImage, D::Dim3D, false, 512),
        Spec(7, K::StorageImage, D::Dim3D, false, 256),
    };
    const ImageDim storageDims[] = {D::Dim2D, D::Dim2D, D::Dim3D};
    const bool storageArrayed[] = {false, true, false};
    const std::uint32_t storageCounts[] = {1024, 256, 256};
    for (std::uint32_t dim = 0; dim < 3; ++dim) {
        for (std::uint32_t component = 0; component < 3; ++component) {
            bindings.push_back(Spec(8 + 3 * dim + component, K::StorageImage, storageDims[dim], storageArrayed[dim], storageCounts[dim]));
        }
    }
    for (std::uint32_t component = 0; component < 3; ++component) {
        bindings.push_back(Spec(17 + component, K::StorageImage, D::Dim1D, false, 256));
    }
    return bindings;
}

}

const std::vector<BindingSpec>& TitleBindings() {
    static const std::vector<BindingSpec> bindings = Build();
    return bindings;
}

const char* KindName(DescriptorKind kind) {
    switch (kind) {
    case DescriptorKind::SampledImage: return "sampled_image";
    case DescriptorKind::Sampler: return "sampler";
    case DescriptorKind::StorageBuffer: return "storage_buffer";
    case DescriptorKind::StorageImage: return "storage_image";
    }
    return "unknown";
}

const char* DimName(ImageDim dim) {
    switch (dim) {
    case ImageDim::None: return "none";
    case ImageDim::Dim1D: return "1d";
    case ImageDim::Dim2D: return "2d";
    case ImageDim::Dim3D: return "3d";
    }
    return "unknown";
}

std::string TitleLayoutJson() {
    std::string out = "{\"format\": \"anybox-title-layout\", \"version\": 1, \"set\": 0, \"pushConstantBytes\": " + std::to_string(kTitlePushConstantBytes) +
                      ", \"bindings\": [";
    bool first = true;
    for (const BindingSpec& spec : TitleBindings()) {
        out += first ? "\n  " : ",\n  ";
        first = false;
        out += "{\"binding\": " + std::to_string(spec.binding) + ", \"type\": \"" + KindName(spec.kind) + "\", \"dim\": \"" + DimName(spec.dim) +
               "\", \"arrayed\": " + (spec.arrayed ? "true" : "false") + ", \"count\": " + std::to_string(spec.count) + "}";
    }
    out += "\n]}\n";
    return out;
}

}
