#include "QueryHeap.hpp"

#include <algorithm>

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

std::uint32_t QueryHeap::ResultBytes(D3D12_QUERY_TYPE type) {
    switch (type) {
    case D3D12_QUERY_TYPE_OCCLUSION:
    case D3D12_QUERY_TYPE_BINARY_OCCLUSION:
    case D3D12_QUERY_TYPE_TIMESTAMP: return 8;
    case D3D12_QUERY_TYPE_PIPELINE_STATISTICS: return sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0:
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1:
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM2:
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3: return sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
    default: return 0;
    }
}

bool QueryHeap::Store(UINT index, const std::uint8_t* result, std::uint32_t bytes) {
    if (index >= desc_.Count) return false;
    const std::lock_guard guard(lock_);
    if (results_.size() != desc_.Count) results_.resize(desc_.Count);
    results_[index].assign(result, result + bytes);
    return true;
}

bool QueryHeap::Load(UINT index, std::uint8_t* result, std::uint32_t bytes) const {
    if (index >= desc_.Count) return false;
    const std::lock_guard guard(lock_);
    std::fill(result, result + bytes, std::uint8_t{0});
    if (index < results_.size()) {
        const std::vector<std::uint8_t>& stored = results_[index];
        std::copy(stored.begin(), stored.begin() + (std::min)(stored.size(), static_cast<std::size_t>(bytes)), result);
    }
    return true;
}

bool QueryHeap::Implements(REFIID riid) const {
    return DeviceChildBase::Implements(riid) || riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12QueryHeap);
}

}
