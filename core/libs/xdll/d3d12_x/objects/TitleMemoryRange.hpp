#pragma once

#include "ObjectBase.hpp"

#include <cstdint>

namespace abx::d3d12x {

class TitleMemoryRange final : public DeviceChildBase<ID3D12Pageable> {
public:
    TitleMemoryRange(IUnknown* parent, std::uint64_t start, std::uint64_t bytes) : DeviceChildBase(parent), start_(start), bytes_(bytes) {}

    std::uint64_t Start() const { return start_; }
    std::uint64_t Bytes() const { return bytes_; }

protected:
    bool Implements(REFIID riid) const override { return DeviceChildBase::Implements(riid) || riid == __uuidof(ID3D12Pageable); }

private:
    std::uint64_t start_;
    std::uint64_t bytes_;
};

}
