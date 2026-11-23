#pragma once

#include "ObjectBase.hpp"
#include "PipelineLibrary.hpp"
#include "RootSignature.hpp"
#include "ShaderStore.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace abx::d3d12x {

struct PipelineContext {
    const ShaderStore& store;
    PipelineLibrary& library;
};

struct GraphicsDescription {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputs;
    std::vector<std::string> semantics;
    std::vector<std::uint8_t> vertex, pixel, geometry, mesh, amplification;
    bool meshPipeline = false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC Rebuilt() const;
    D3D12_SHADER_BYTECODE Mesh() const { return {mesh.empty() ? nullptr : mesh.data(), mesh.size()}; }
    D3D12_SHADER_BYTECODE Amplification() const { return {amplification.empty() ? nullptr : amplification.data(), amplification.size()}; }
};

class PipelineStateObject final : public DeviceChildBase<ID3D12PipelineState> {
public:
    static HRESULT CreateCompute(const PipelineContext& context, IUnknown* parent, const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, REFIID riid, void** state, std::string& error);
    static HRESULT CreateGraphics(const PipelineContext& context, IUnknown* parent, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, REFIID riid, void** state,
                                  std::string& error);
    static HRESULT CreateMesh(const PipelineContext& context, IUnknown* parent, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, const D3D12_SHADER_BYTECODE& mesh,
                              const D3D12_SHADER_BYTECODE& amplification, REFIID riid, void** state, std::string& error);
    static HRESULT CreateUnbuilt(IUnknown* parent, VkPipelineBindPoint point, ID3D12RootSignature* signature, std::string reason, REFIID riid, void** state,
                                 std::string& error);

    HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob** blob) override;

    VkPipelineBindPoint BindPoint() const { return point_; }
    VkPipeline Pipeline() const { return pipeline_; }
    RootSignature* Signature() const { return signature_; }
    PipelineOrigin Origin() const { return origin_; }
    const std::string& UnbuiltReason() const { return unbuilt_; }
    const std::vector<std::string>& Shaders() const { return shaders_; }
    bool UsesCodeBase() const { return usesCodeBase_; }
    VkPolygonMode PolygonMode() const { return polygonMode_; }
    bool HasStage(const char* stage) const;
    const GraphicsDescription* Description() const { return description_.get(); }

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
    std::string unbuilt_;
    std::vector<std::string> shaders_;
    bool usesCodeBase_ = false;
    std::shared_ptr<const GraphicsDescription> description_;
    VkPolygonMode polygonMode_ = VK_POLYGON_MODE_FILL;
};

}
