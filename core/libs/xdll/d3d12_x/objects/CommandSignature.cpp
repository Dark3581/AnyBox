#include "CommandSignature.hpp"

namespace abx::d3d12x {

HRESULT CommandSignature::Create(IUnknown* parent, const D3D12_COMMAND_SIGNATURE_DESC& desc, REFIID riid, void** signature, std::string& error) {
    if (signature == nullptr) {
        error = "no output for the command signature";
        return E_POINTER;
    }
    *signature = nullptr;
    CommandSignature* created = new CommandSignature(parent);
    std::string unsupported;
    created->signature_ = TranslateCommandSignature(desc, unsupported);
    created->unsupported_ = unsupported;
    const HRESULT result = created->QueryInterface(riid, signature);
    created->Release();
    if (FAILED(result)) error = "the command signature does not implement the requested interface";
    return result;
}

bool CommandSignature::Implements(REFIID riid) const {
    return DeviceChildBase::Implements(riid) || riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12CommandSignature);
}

}
