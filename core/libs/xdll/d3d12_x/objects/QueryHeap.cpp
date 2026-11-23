#include "QueryHeap.hpp"

namespace abx::d3d12x {

HRESULT QueryHeap::Create(IUnknown* parent, const D3D12_QUERY_HEAP_DESC& desc, REFIID riid, void** heap, std::string& error) {
    if (heap == nullptr) {
        error = "no output for the query heap";
        return E_POINTER;
    }
    *heap = nullptr;
    if (desc.Count == 0 || desc.NodeMask > 1) {
        error = "a query heap needs at least one query and the first GPU node";
        return E_INVALIDARG;
    }
    QueryHeap* created = new QueryHeap(parent, desc);
    const HRESULT result = created->QueryInterface(riid, heap);
    created->Release();
    if (FAILED(result)) error = "the query heap does not implement the requested interface";
    return result;
}

bool QueryHeap::Implements(REFIID riid) const {
    return DeviceChildBase::Implements(riid) || riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12QueryHeap);
}

}
