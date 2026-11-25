#pragma once

#include "ObjectBase.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace abx::d3d12x {

class QueryHeap final : public DeviceChildBase<ID3D12QueryHeap> {
public:
    static HRESULT Create(IUnknown* parent, const D3D12_QUERY_HEAP_DESC& desc, REFIID riid, void** heap, std::string& error);

    const D3D12_QUERY_HEAP_DESC& Desc() const { return desc_; }
    static std::uint32_t ResultBytes(D3D12_QUERY_TYPE type);
    bool Store(UINT index, const std::uint8_t* result, std::uint32_t bytes);
    bool Load(UINT index, std::uint8_t* result, std::uint32_t bytes) const;

protected:
    bool Implements(REFIID riid) const override;

private:
    QueryHeap(IUnknown* parent, const D3D12_QUERY_HEAP_DESC& desc) : DeviceChildBase(parent), desc_(desc) {}

    D3D12_QUERY_HEAP_DESC desc_;
    mutable std::mutex lock_;
    std::vector<std::vector<std::uint8_t>> results_;
};

}
