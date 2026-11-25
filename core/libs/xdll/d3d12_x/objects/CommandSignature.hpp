#pragma once

#include "IndirectCommands.hpp"
#include "ObjectBase.hpp"

#include <optional>
#include <string>

namespace abx::d3d12x {

class CommandSignature final : public DeviceChildBase<ID3D12CommandSignature> {
public:
    static HRESULT Create(IUnknown* parent, const D3D12_COMMAND_SIGNATURE_DESC& desc, REFIID riid, void** signature, std::string& error);

    const std::optional<IndirectSignature>& Signature() const { return signature_; }
    const std::string& Unsupported() const { return unsupported_; }

protected:
    bool Implements(REFIID riid) const override;

private:
    explicit CommandSignature(IUnknown* parent) : DeviceChildBase(parent) {}

    std::optional<IndirectSignature> signature_;
    std::string unsupported_;
};

}
