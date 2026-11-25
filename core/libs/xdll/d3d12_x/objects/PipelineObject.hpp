#pragma once

#include "ObjectBase.hpp"
#include "PipelineLibrary.hpp"
#include "RootSignature.hpp"
#include "ShaderStore.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
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
    bool Alive() const { return alive_ == kAlive; }
    static HRESULT CreateCompute(const PipelineContext& context, IUnknown* parent, const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, REFIID riid, void** state, std::string& error);
    static HRESULT CreateGraphics(const PipelineContext& context, IUnknown* parent, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, REFIID riid, void** state,
                                  std::string& error);
    static HRESULT CreateMesh(const PipelineContext& context, IUnknown* parent, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, const D3D12_SHADER_BYTECODE& mesh,
                              const D3D12_SHADER_BYTECODE& amplification, REFIID riid, void** state, std::string& error);
    static HRESULT CreateUnbuilt(IUnknown* parent, VkPipelineBindPoint point, ID3D12RootSignature* signature, std::string reason, REFIID riid, void** state,
                                 std::string& error);

    HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob** blob) override;

    VkPipelineBindPoint BindPoint() const { return point_; }
    VkPipeline Pipeline() const { return pipeline_.load(std::memory_order_acquire); }
    bool Pending() const { return pending_.load(std::memory_order_acquire); }
    void Prioritize();
    void BuildNow();
    RootSignature* Signature() const { return signature_; }
    PipelineOrigin Origin() const { return origin_; }
    std::string UnbuiltReason() const { return Pending() ? std::string("its pipeline is still being built in the background") : unbuilt_; }
    const std::vector<std::string>& Shaders() const { return shaders_; }
    bool UsesCodeBase() const { return usesCodeBase_; }
    VkPolygonMode PolygonMode() const { return polygonMode_; }
    bool HasStage(const char* stage) const;
    const GraphicsDescription* Description() const { return description_.get(); }

protected:
    bool Implements(REFIID riid) const override;

private:
    static constexpr std::uint32_t kAlive = 0x50534F31u;
    PipelineStateObject(IUnknown* parent, VkPipelineBindPoint point, VkPipeline pipeline, PipelineOrigin origin, RootSignature* signature);
    ~PipelineStateObject() override;
    static RootSignature* SignatureOf(ID3D12RootSignature* signature, std::string& error);
    static HRESULT Finish(PipelineStateObject* created, REFIID riid, void** state, std::string& error);
    void BuildInBackground(std::function<PipelineEntry()> build, std::uint64_t moduleBytes);
    void RunBuild(bool cacheOnly = false);

    std::uint32_t alive_ = kAlive;
    VkPipelineBindPoint point_;
    std::atomic<VkPipeline> pipeline_;
    std::atomic<bool> pending_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> prioritized_{false};
    bool heavy_ = false;
    std::uint64_t moduleBytes_ = 0;
    std::function<PipelineEntry()> build_;
    PipelineOrigin origin_;
    RootSignature* signature_;
    std::string unbuilt_;
    std::vector<std::string> shaders_;
    bool usesCodeBase_ = false;
    std::shared_ptr<const GraphicsDescription> description_;
    VkPolygonMode polygonMode_ = VK_POLYGON_MODE_FILL;
};

}
