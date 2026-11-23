#pragma once

#include "ObjectBase.hpp"
#include "PipelineLibrary.hpp"
#include "RootSignature.hpp"
#include "ShaderStore.hpp"

#include <string>

namespace abx::d3d12x {

struct PipelineContext {
    const ShaderStore& store;
    PipelineLibrary& library;
};

class PipelineStateObject final : public DeviceChildBase<ID3D12PipelineState> {
public:
    static HRESULT CreateCompute(const PipelineContext& context, IUnknown* parent, const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, REFIID riid, void** state, std::string& error);
    static HRESULT CreateGraphics(const PipelineContext& context, IUnknown* parent, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, REFIID riid, void** state,
                                  std::string& error);

    HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob** blob) override;

    VkPipelineBindPoint BindPoint() const { return point_; }
    VkPipeline Pipeline() const { return pipeline_; }
    RootSignature* Signature() const { return signature_; }
    PipelineOrigin Origin() const { return origin_; }

protected:
    bool Implements(REFIID riid) const override;

private:
    PipelineStateObject(IUnknown* parent, VkPipelineBindPoint point, VkPipeline pipeline, PipelineOrigin origin, RootSignature* signature);
    ~PipelineStateObject() override;
    static RootSignature* SignatureOf(ID3D12RootSignature* signature, std::string& error);
    static HRESULT Finish(PipelineStateObject* created, REFIID riid, void** state, std::string& error);

    VkPipelineBindPoint point_;
    VkPipeline pipeline_;
    PipelineOrigin origin_;
    RootSignature* signature_;
};

}
