#include "xdll/d3d12_x/d3d12_x.hpp"

#include <cstddef>
#include <cstdint>

namespace {

constexpr std::uint32_t ConsoleFlagBits = 0xFF000000u;

D3D12_ROOT_SIGNATURE_FLAGS DesktopFlags(D3D12_ROOT_SIGNATURE_FLAGS flags) {
    return static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(static_cast<std::uint32_t>(flags) & ~ConsoleFlagBits);
}

HMODULE HostD3D12() {
    static const HMODULE module = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module;
}

template <typename Function>
Function HostFunction(const char* function) {
    const HMODULE module = HostD3D12();
    if (module == nullptr) {
        AbxUnsupported(function, "the host has no d3d12.dll");
    }
    const FARPROC address = GetProcAddress(module, function);
    if (address == nullptr) {
        AbxUnsupported(function, "the host d3d12.dll does not export this function");
    }
    return reinterpret_cast<Function>(reinterpret_cast<void*>(address));
}

}

ABX_API HRESULT D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* rootSignature, D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob** blob, ID3DBlob** errorBlob) {
    const auto serialize = HostFunction<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(__func__);
    HRESULT result = E_INVALIDARG;
    if (rootSignature == nullptr) {
        result = serialize(rootSignature, version, blob, errorBlob);
        AbxTrace("D3D12SerializeRootSignature(null, %d) -> 0x%08lx", static_cast<int>(version), static_cast<unsigned long>(result));
        return result;
    }
    D3D12_ROOT_SIGNATURE_DESC desktop = *rootSignature;
    desktop.Flags = DesktopFlags(desktop.Flags);
    result = serialize(&desktop, version, blob, errorBlob);
    AbxTrace("D3D12SerializeRootSignature(%p, %d) parameters %u, samplers %u, flags 0x%x -> 0x%x, result 0x%08lx", static_cast<const void*>(rootSignature),
             static_cast<int>(version), rootSignature->NumParameters, rootSignature->NumStaticSamplers, static_cast<unsigned>(rootSignature->Flags),
             static_cast<unsigned>(desktop.Flags), static_cast<unsigned long>(result));
    return result;
}

ABX_API HRESULT D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* rootSignature, ID3DBlob** blob, ID3DBlob** errorBlob) {
    const auto serialize = HostFunction<PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE>(__func__);
    HRESULT result = E_INVALIDARG;
    if (rootSignature == nullptr) {
        result = serialize(rootSignature, blob, errorBlob);
        AbxTrace("D3D12SerializeVersionedRootSignature(null) -> 0x%08lx", static_cast<unsigned long>(result));
        return result;
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desktop = *rootSignature;
    D3D12_ROOT_SIGNATURE_FLAGS flags{};
    if (desktop.Version == D3D_ROOT_SIGNATURE_VERSION_1_0) {
        flags = desktop.Desc_1_0.Flags;
        desktop.Desc_1_0.Flags = DesktopFlags(flags);
    } else if (desktop.Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
        flags = desktop.Desc_1_1.Flags;
        desktop.Desc_1_1.Flags = DesktopFlags(flags);
    } else {
        AbxUnsupported(__func__, "root signature version is neither 1.0 nor 1.1");
    }
    result = serialize(&desktop, blob, errorBlob);
    AbxTrace("D3D12SerializeVersionedRootSignature(%p) version 0x%x, flags 0x%x -> 0x%x, result 0x%08lx", static_cast<const void*>(rootSignature),
             static_cast<unsigned>(desktop.Version), static_cast<unsigned>(flags), static_cast<unsigned>(DesktopFlags(flags)), static_cast<unsigned long>(result));
    return result;
}

ABX_API HRESULT D3D12CreateRootSignatureDeserializer(const void* data, SIZE_T size, REFIID iid, void** deserializer) {
    const HRESULT result = HostFunction<PFN_D3D12_CREATE_ROOT_SIGNATURE_DESERIALIZER>(__func__)(data, size, iid, deserializer);
    AbxTrace("D3D12CreateRootSignatureDeserializer(%p, %zu) -> 0x%08lx", data, static_cast<std::size_t>(size), static_cast<unsigned long>(result));
    return result;
}

ABX_API HRESULT D3D12CreateVersionedRootSignatureDeserializer(const void* data, SIZE_T size, REFIID iid, void** deserializer) {
    const HRESULT result = HostFunction<PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER>(__func__)(data, size, iid, deserializer);
    AbxTrace("D3D12CreateVersionedRootSignatureDeserializer(%p, %zu) -> 0x%08lx", data, static_cast<std::size_t>(size), static_cast<unsigned long>(result));
    return result;
}
