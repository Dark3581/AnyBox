#include "PipelineObject.hpp"

#include "PipelineState.hpp"

#include <optional>

namespace abx::d3d12x {

namespace {

HRESULT FromEntry(const PipelineEntry& entry, std::string& error) {
    if (entry.pipeline != VK_NULL_HANDLE) {
        return S_OK;
    }
    error = std::string("the pipeline was ") + PipelineOriginName(entry.origin) + (entry.error.empty() ? "" : ": " + entry.error);
    return entry.origin == PipelineOrigin::Refused ? E_OUTOFMEMORY : E_FAIL;
}

}

PipelineStateObject::PipelineStateObject(IUnknown* parent, VkPipelineBindPoint point, VkPipeline pipeline, PipelineOrigin origin, RootSignature* signature)
    : DeviceChildBase(parent), point_(point), pipeline_(pipeline), origin_(origin), signature_(signature) {
    signature_->AddRef();
}

PipelineStateObject::~PipelineStateObject() {
    signature_->Release();
}

bool PipelineStateObject::Implements(REFIID riid) const {
    return DeviceChildBase::Implements(riid) || riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12PipelineState);
}

HRESULT STDMETHODCALLTYPE PipelineStateObject::GetCachedBlob(ID3DBlob** blob) {
    if (blob != nullptr) *blob = nullptr;
    return E_NOTIMPL;
}

RootSignature* PipelineStateObject::SignatureOf(ID3D12RootSignature* signature, std::string& error) {
    if (signature == nullptr) {
        error = "a pipeline state without a root signature: reading one embedded in the shader is not implemented";
        return nullptr;
    }
    RootSignature* ours = dynamic_cast<RootSignature*>(signature);
    if (ours == nullptr) error = "a root signature that AnyBox did not create";
    return ours;
}

namespace {

const char* StageName(VkShaderStageFlagBits stage) {
    switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT: return "vs";
    case VK_SHADER_STAGE_FRAGMENT_BIT: return "ps";
    case VK_SHADER_STAGE_GEOMETRY_BIT: return "gs";
    case VK_SHADER_STAGE_MESH_BIT_EXT: return "ms";
    case VK_SHADER_STAGE_TASK_BIT_EXT: return "as";
    default: return "?";
    }
}

}

bool PipelineStateObject::HasStage(const char* stage) const {
    const std::string prefix = std::string(stage) + " ";
    for (const std::string& shader : shaders_) {
        if (shader.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

HRESULT PipelineStateObject::Finish(PipelineStateObject* created, REFIID riid, void** state, std::string& error) {
    const HRESULT result = created->QueryInterface(riid, state);
    created->Release();
    if (FAILED(result)) error = "the pipeline state does not implement the requested interface";
    return result;
}

HRESULT PipelineStateObject::CreateCompute(const PipelineContext& context, IUnknown* parent, const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, REFIID riid, void** state,
                                           std::string& error) {
    if (state == nullptr) {
        error = "no output for the pipeline state";
        return E_POINTER;
    }
    *state = nullptr;
    RootSignature* signature = SignatureOf(desc.pRootSignature, error);
    if (signature == nullptr) {
        return E_INVALIDARG;
    }
    const std::optional<PipelineSource> source = ComputePipelineSource(desc, context.store, error);
    if (!source) {
        return E_INVALIDARG;
    }
    const PipelineEntry entry = context.library.Compute(*source);
    const HRESULT built = FromEntry(entry, error);
    if (FAILED(built)) {
        return built;
    }
    PipelineStateObject* created = new PipelineStateObject(parent, VK_PIPELINE_BIND_POINT_COMPUTE, entry.pipeline, entry.origin, signature);
    created->shaders_.push_back("cs " + source->key);
    created->usesCodeBase_ = entry.usesCodeBase;
    return Finish(created, riid, state, error);
}

HRESULT PipelineStateObject::CreateMesh(const PipelineContext& context, IUnknown* parent, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, const D3D12_SHADER_BYTECODE& mesh,
                                        const D3D12_SHADER_BYTECODE& amplification, REFIID riid, void** state, std::string& error) {
    if (state == nullptr) {
        error = "no output for the pipeline state";
        return E_POINTER;
    }
    *state = nullptr;
    RootSignature* signature = SignatureOf(desc.pRootSignature, error);
    if (signature == nullptr) {
        return E_INVALIDARG;
    }
    const std::optional<GraphicsRequest> request = MeshPipelineRequest(desc, mesh, amplification, context.store, error);
    if (!request) {
        return E_INVALIDARG;
    }
    const PipelineEntry entry = context.library.Graphics(*request);
    const HRESULT built = FromEntry(entry, error);
    if (FAILED(built)) {
        return built;
    }
    PipelineStateObject* created = new PipelineStateObject(parent, VK_PIPELINE_BIND_POINT_GRAPHICS, entry.pipeline, entry.origin, signature);
    for (const GraphicsShaderSource& shader : request->shaders) created->shaders_.push_back(StageName(shader.stage) + std::string(" ") + shader.key);
    created->usesCodeBase_ = entry.usesCodeBase;
    created->polygonMode_ = request->state.polygonMode;
    return Finish(created, riid, state, error);
}

HRESULT PipelineStateObject::CreateUnbuilt(IUnknown* parent, VkPipelineBindPoint point, ID3D12RootSignature* signature, std::string reason, REFIID riid, void** state,
                                           std::string& error) {
    if (state == nullptr) {
        error = "no output for the pipeline state";
        return E_POINTER;
    }
    *state = nullptr;
    RootSignature* ours = SignatureOf(signature, error);
    if (ours == nullptr) {
        return E_INVALIDARG;
    }
    PipelineStateObject* created = new PipelineStateObject(parent, point, VK_NULL_HANDLE, PipelineOrigin{}, ours);
    created->unbuilt_ = std::move(reason);
    return Finish(created, riid, state, error);
}

HRESULT PipelineStateObject::CreateGraphics(const PipelineContext& context, IUnknown* parent, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, REFIID riid, void** state,
                                            std::string& error) {
    if (state == nullptr) {
        error = "no output for the pipeline state";
        return E_POINTER;
    }
    *state = nullptr;
    RootSignature* signature = SignatureOf(desc.pRootSignature, error);
    if (signature == nullptr) {
        return E_INVALIDARG;
    }
    const std::optional<GraphicsRequest> request = GraphicsPipelineRequest(desc, context.store, error);
    if (!request) {
        return E_INVALIDARG;
    }
    const PipelineEntry entry = context.library.Graphics(*request);
    const HRESULT built = FromEntry(entry, error);
    if (FAILED(built)) {
        return built;
    }
    PipelineStateObject* created = new PipelineStateObject(parent, VK_PIPELINE_BIND_POINT_GRAPHICS, entry.pipeline, entry.origin, signature);
    for (const GraphicsShaderSource& shader : request->shaders) created->shaders_.push_back(StageName(shader.stage) + std::string(" ") + shader.key);
    created->usesCodeBase_ = entry.usesCodeBase;
    created->polygonMode_ = request->state.polygonMode;
    return Finish(created, riid, state, error);
}

}
