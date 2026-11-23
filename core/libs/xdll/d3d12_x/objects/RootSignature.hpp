#pragma once

#include "ObjectBase.hpp"
#include "RootSignatureLayout.hpp"

#include <string>
#include <utility>

namespace abx::d3d12x {

class RootSignature final : public DeviceChildBase<ID3D12RootSignature> {
public:
    static HRESULT Create(IUnknown* parent, const void* blob, SIZE_T bytes, REFIID riid, void** signature, std::string& error);

    const RootSignatureLayout& Layout() const { return layout_; }

protected:
    bool Implements(REFIID riid) const override;

private:
    RootSignature(IUnknown* parent, RootSignatureLayout layout) : DeviceChildBase(parent), layout_(std::move(layout)) {}

    RootSignatureLayout layout_;
};

}
