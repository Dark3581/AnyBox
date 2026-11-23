#pragma once

#include "ObjectBase.hpp"

#include <string>

namespace abx::d3d12x {

class QueryHeap final : public DeviceChildBase<ID3D12QueryHeap> {
public:
    static HRESULT Create(IUnknown* parent, const D3D12_QUERY_HEAP_DESC& desc, REFIID riid, void** heap, std::string& error);

    const D3D12_QUERY_HEAP_DESC& Desc() const { return desc_; }

protected:
    bool Implements(REFIID riid) const override;

private:
    QueryHeap(IUnknown* parent, const D3D12_QUERY_HEAP_DESC& desc) : DeviceChildBase(parent), desc_(desc) {}

    D3D12_QUERY_HEAP_DESC desc_;
};

}
