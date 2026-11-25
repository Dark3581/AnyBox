#include "RootSignature.hpp"

#include <optional>
#include <utility>

namespace abx::d3d12x {

HRESULT RootSignature::Create(IUnknown* parent, const void* blob, SIZE_T bytes, REFIID riid, void** signature, std::string& error) {
    if (signature == nullptr) {
        error = "no output for the root signature";
        return E_POINTER;
    }
    *signature = nullptr;
    if (blob == nullptr || bytes == 0) {
        error = "a root signature without a serialized blob";
        return E_INVALIDARG;
    }
    std::optional<RootSignatureLayout> layout = DeserializeRootSignature(blob, bytes, error);
    if (!layout) {
        return E_INVALIDARG;
    }
    layout->heapIndexing = HeapIndexingOf(blob, bytes);
    RootSignature* created = new RootSignature(parent, std::move(*layout));
    const HRESULT result = created->QueryInterface(riid, signature);
    created->Release();
    if (FAILED(result)) error = "the root signature does not implement the requested interface";
    return result;
}

bool RootSignature::Implements(REFIID riid) const {
    return DeviceChildBase::Implements(riid) || riid == __uuidof(ID3D12RootSignature);
}

}
