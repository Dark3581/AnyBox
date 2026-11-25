#include "RootSignatureLayout.hpp"

#include <unordered_map>

#include <mutex>

namespace abx::d3d12x {

namespace {

constexpr UINT kKnownRootFlags = 0xFFFu;
constexpr UINT kUnbounded = 0xFFFFFFFFu;

bool ValidVisibility(D3D12_SHADER_VISIBILITY visibility) {
    switch (visibility) {
    case D3D12_SHADER_VISIBILITY_ALL:
    case D3D12_SHADER_VISIBILITY_VERTEX:
    case D3D12_SHADER_VISIBILITY_HULL:
    case D3D12_SHADER_VISIBILITY_DOMAIN:
    case D3D12_SHADER_VISIBILITY_GEOMETRY:
    case D3D12_SHADER_VISIBILITY_PIXEL:
    case D3D12_SHADER_VISIBILITY_AMPLIFICATION:
    case D3D12_SHADER_VISIBILITY_MESH:
        return true;
    default:
        return false;
    }
}

bool CheckTable(const std::vector<D3D12_DESCRIPTOR_RANGE1>& ranges, UINT parameter, std::string& error) {
    if (ranges.empty()) {
        error = "descriptor table " + std::to_string(parameter) + " has no ranges";
        return false;
    }
    bool samplers = false;
    bool views = false;
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        const D3D12_DESCRIPTOR_RANGE1& range = ranges[index];
        switch (range.RangeType) {
        case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
        case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
        case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
            views = true;
            break;
        case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
            samplers = true;
            break;
        default:
            error = "descriptor table " + std::to_string(parameter) + " has range type " + std::to_string(static_cast<int>(range.RangeType));
            return false;
        }
        if (range.NumDescriptors == 0) {
            error = "descriptor table " + std::to_string(parameter) + " has an empty range";
            return false;
        }
        if (range.NumDescriptors == kUnbounded && index + 1 != ranges.size()) {
            error = "descriptor table " + std::to_string(parameter) + " has an unbounded range before its last range";
            return false;
        }
    }
    if (samplers && views) {
        error = "descriptor table " + std::to_string(parameter) + " mixes samplers with other descriptors";
        return false;
    }
    return true;
}

}

std::optional<D3D12_SAMPLER_DESC> StaticSamplerDesc(const D3D12_STATIC_SAMPLER_DESC& desc, std::string& error) {
    D3D12_SAMPLER_DESC sampler{desc.Filter, desc.AddressU, desc.AddressV, desc.AddressW, desc.MipLODBias, desc.MaxAnisotropy, desc.ComparisonFunc, {0.0f, 0.0f, 0.0f, 0.0f},
                               desc.MinLOD, desc.MaxLOD};
    switch (desc.BorderColor) {
    case D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK:
        break;
    case D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK:
        sampler.BorderColor[3] = 1.0f;
        break;
    case D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE:
        sampler.BorderColor[0] = sampler.BorderColor[1] = sampler.BorderColor[2] = sampler.BorderColor[3] = 1.0f;
        break;
    default:
        error = "static border color " + std::to_string(static_cast<int>(desc.BorderColor)) + " is not supported";
        return std::nullopt;
    }
    return sampler;
}

std::optional<RootSignatureLayout> BuildRootSignatureLayout(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc, std::string& error) {
    RootSignatureLayout layout;
    UINT parameterCount = 0;
    UINT samplerCount = 0;
    const D3D12_STATIC_SAMPLER_DESC* samplers = nullptr;
    if (desc.Version == D3D_ROOT_SIGNATURE_VERSION_1_0) {
        layout.flags = desc.Desc_1_0.Flags;
        parameterCount = desc.Desc_1_0.NumParameters;
        samplerCount = desc.Desc_1_0.NumStaticSamplers;
        samplers = desc.Desc_1_0.pStaticSamplers;
        if (parameterCount != 0 && desc.Desc_1_0.pParameters == nullptr) {
            error = "the root signature names parameters but has no parameter array";
            return std::nullopt;
        }
    } else if (desc.Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
        layout.flags = desc.Desc_1_1.Flags;
        parameterCount = desc.Desc_1_1.NumParameters;
        samplerCount = desc.Desc_1_1.NumStaticSamplers;
        samplers = desc.Desc_1_1.pStaticSamplers;
        if (parameterCount != 0 && desc.Desc_1_1.pParameters == nullptr) {
            error = "the root signature names parameters but has no parameter array";
            return std::nullopt;
        }
    } else {
        error = "root signature version " + std::to_string(static_cast<int>(desc.Version)) + " is not 1.0 or 1.1";
        return std::nullopt;
    }
    if ((static_cast<UINT>(layout.flags) & ~kKnownRootFlags) != 0) {
        error = "root signature flags " + std::to_string(static_cast<UINT>(layout.flags) & ~kKnownRootFlags) + " are unknown";
        return std::nullopt;
    }
    for (UINT index = 0; index < parameterCount; ++index) {
        RootParameterLayout parameter;
        if (desc.Version == D3D_ROOT_SIGNATURE_VERSION_1_0) {
            const D3D12_ROOT_PARAMETER& source = desc.Desc_1_0.pParameters[index];
            parameter.type = source.ParameterType;
            parameter.visibility = source.ShaderVisibility;
            if (source.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                if (source.DescriptorTable.NumDescriptorRanges != 0 && source.DescriptorTable.pDescriptorRanges == nullptr) {
                    error = "descriptor table " + std::to_string(index) + " names ranges but has no range array";
                    return std::nullopt;
                }
                for (UINT range = 0; range < source.DescriptorTable.NumDescriptorRanges; ++range) {
                    const D3D12_DESCRIPTOR_RANGE& old = source.DescriptorTable.pDescriptorRanges[range];
                    parameter.ranges.push_back({old.RangeType, old.NumDescriptors, old.BaseShaderRegister, old.RegisterSpace, D3D12_DESCRIPTOR_RANGE_FLAG_NONE,
                                                old.OffsetInDescriptorsFromTableStart});
                }
            } else if (source.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
                parameter.shaderRegister = source.Constants.ShaderRegister;
                parameter.registerSpace = source.Constants.RegisterSpace;
                parameter.constants = source.Constants.Num32BitValues;
            } else {
                parameter.shaderRegister = source.Descriptor.ShaderRegister;
                parameter.registerSpace = source.Descriptor.RegisterSpace;
            }
        } else {
            const D3D12_ROOT_PARAMETER1& source = desc.Desc_1_1.pParameters[index];
            parameter.type = source.ParameterType;
            parameter.visibility = source.ShaderVisibility;
            if (source.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                if (source.DescriptorTable.NumDescriptorRanges != 0 && source.DescriptorTable.pDescriptorRanges == nullptr) {
                    error = "descriptor table " + std::to_string(index) + " names ranges but has no range array";
                    return std::nullopt;
                }
                parameter.ranges.assign(source.DescriptorTable.pDescriptorRanges, source.DescriptorTable.pDescriptorRanges + source.DescriptorTable.NumDescriptorRanges);
            } else if (source.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
                parameter.shaderRegister = source.Constants.ShaderRegister;
                parameter.registerSpace = source.Constants.RegisterSpace;
                parameter.constants = source.Constants.Num32BitValues;
            } else {
                parameter.shaderRegister = source.Descriptor.ShaderRegister;
                parameter.registerSpace = source.Descriptor.RegisterSpace;
                parameter.descriptorFlags = source.Descriptor.Flags;
            }
        }
        if (!ValidVisibility(parameter.visibility)) {
            error = "root parameter " + std::to_string(index) + " has shader visibility " + std::to_string(static_cast<int>(parameter.visibility));
            return std::nullopt;
        }
        switch (parameter.type) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            if (!CheckTable(parameter.ranges, index, error)) {
                return std::nullopt;
            }
            parameter.words = 1;
            break;
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            if (parameter.constants == 0) {
                error = "root constants " + std::to_string(index) + " have no values";
                return std::nullopt;
            }
            parameter.words = parameter.constants;
            break;
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
        case D3D12_ROOT_PARAMETER_TYPE_UAV:
            parameter.words = 2;
            break;
        default:
            error = "root parameter " + std::to_string(index) + " has type " + std::to_string(static_cast<int>(parameter.type));
            return std::nullopt;
        }
        parameter.firstWord = layout.words;
        if (parameter.words > kRootSignatureWordLimit - layout.words) {
            error = "the root signature needs more than " + std::to_string(kRootSignatureWordLimit) + " 32-bit words";
            return std::nullopt;
        }
        layout.words += parameter.words;
        layout.parameters.push_back(std::move(parameter));
    }
    if (samplerCount != 0 && samplers == nullptr) {
        error = "the root signature names static samplers but has no sampler array";
        return std::nullopt;
    }
    for (UINT index = 0; index < samplerCount; ++index) {
        const std::optional<D3D12_SAMPLER_DESC> sampler = StaticSamplerDesc(samplers[index], error);
        const std::optional<Sampler> translated = sampler ? TranslateSampler(*sampler, error) : std::nullopt;
        if (!translated || !ValidVisibility(samplers[index].ShaderVisibility)) {
            if (translated) error = "static sampler " + std::to_string(index) + " has an unknown shader visibility";
            else error = "static sampler " + std::to_string(index) + ": " + error;
            return std::nullopt;
        }
        layout.staticSamplers.push_back({samplers[index], *translated});
    }
    return layout;
}

namespace {

std::uint64_t BlobKey(const void* blob, std::size_t bytes) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    const auto* data = static_cast<const std::uint8_t*>(blob);
    for (std::size_t index = 0; index < bytes; ++index) hash = (hash ^ data[index]) * 0x100000001b3ull;
    return hash ^ bytes;
}

std::mutex& HeapIndexingLock() {
    static std::mutex lock;
    return lock;
}

std::unordered_map<std::uint64_t, std::uint32_t>& HeapIndexingTable() {
    static auto* table = new std::unordered_map<std::uint64_t, std::uint32_t>();
    return *table;
}

}

void RememberHeapIndexing(const void* blob, std::size_t bytes, std::uint32_t flags) {
    if (blob == nullptr || bytes == 0) return;
    const std::lock_guard guard(HeapIndexingLock());
    HeapIndexingTable()[BlobKey(blob, bytes)] = flags & kHeapIndexingFlags;
}

std::uint32_t HeapIndexingOf(const void* blob, std::size_t bytes) {
    if (blob == nullptr || bytes == 0) return 0;
    const std::lock_guard guard(HeapIndexingLock());
    const auto found = HeapIndexingTable().find(BlobKey(blob, bytes));
    return found != HeapIndexingTable().end() ? found->second : 0;
}

std::optional<RootSignatureLayout> DeserializeRootSignature(const void* blob, std::size_t bytes, std::string& error) {
    if (blob == nullptr || bytes == 0) {
        error = "the root signature blob is empty";
        return std::nullopt;
    }
    static const HMODULE host = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (host == nullptr) {
        error = "the host has no d3d12.dll";
        return std::nullopt;
    }
    const auto create = reinterpret_cast<PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER>(
        reinterpret_cast<void*>(GetProcAddress(host, "D3D12CreateVersionedRootSignatureDeserializer")));
    if (create == nullptr) {
        error = "the host d3d12.dll has no versioned root signature deserializer";
        return std::nullopt;
    }
    ID3D12VersionedRootSignatureDeserializer* deserializer = nullptr;
    const HRESULT created = create(blob, bytes, __uuidof(ID3D12VersionedRootSignatureDeserializer), reinterpret_cast<void**>(&deserializer));
    if (FAILED(created) || deserializer == nullptr) {
        error = "the host d3d12.dll rejected the root signature blob (" + std::to_string(static_cast<unsigned long>(created)) + ")";
        return std::nullopt;
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc = nullptr;
    const HRESULT converted = deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &desc);
    std::optional<RootSignatureLayout> layout;
    if (FAILED(converted) || desc == nullptr) {
        error = "the host d3d12.dll could not convert the root signature to version 1.1";
    } else {
        layout = BuildRootSignatureLayout(*desc, error);
    }
    deserializer->Release();
    return layout;
}

}
