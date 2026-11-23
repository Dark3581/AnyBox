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

std::vector<std::uint8_t> CopyBytecode(const D3D12_SHADER_BYTECODE& code) {
    if (code.pShaderBytecode == nullptr || code.BytecodeLength == 0) return {};
    const auto* bytes = static_cast<const std::uint8_t*>(code.pShaderBytecode);
    return std::vector<std::uint8_t>(bytes, bytes + code.BytecodeLength);
}

D3D12_SHADER_BYTECODE View(const std::vector<std::uint8_t>& bytes) {
    return {bytes.empty() ? nullptr : bytes.data(), bytes.size()};
}

std::shared_ptr<const GraphicsDescription> Describe(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, const D3D12_SHADER_BYTECODE* mesh, const D3D12_SHADER_BYTECODE* amplification) {
    auto described = std::make_shared<GraphicsDescription>();
    described->desc = desc;
    described->vertex = CopyBytecode(desc.VS);
    described->pixel = CopyBytecode(desc.PS);
    described->geometry = CopyBytecode(desc.GS);
    if (mesh != nullptr) {
        described->mesh = CopyBytecode(*mesh);
        described->meshPipeline = true;
    }
    if (amplification != nullptr) described->amplification = CopyBytecode(*amplification);
    for (UINT index = 0; index < desc.InputLayout.NumElements && desc.InputLayout.pInputElementDescs != nullptr; ++index) {
        described->inputs.push_back(desc.InputLayout.pInputElementDescs[index]);
        described->semantics.emplace_back(desc.InputLayout.pInputElementDescs[index].SemanticName != nullptr ? desc.InputLayout.pInputElementDescs[index].SemanticName : "");
    }
    for (std::size_t index = 0; index < described->inputs.size(); ++index) described->inputs[index].SemanticName = described->semantics[index].c_str();
    described->desc.CachedPSO = {};
    described->desc.StreamOutput = {};
    return described;
}

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

D3D12_GRAPHICS_PIPELINE_STATE_DESC GraphicsDescription::Rebuilt() const {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC rebuilt = desc;
    rebuilt.VS = View(vertex);
    rebuilt.PS = View(pixel);
    rebuilt.GS = View(geometry);
    rebuilt.HS = {};
    rebuilt.DS = {};
    rebuilt.InputLayout = {inputs.empty() ? nullptr : inputs.data(), static_cast<UINT>(inputs.size())};
    return rebuilt;
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
    created->description_ = Describe(desc, &mesh, amplification.pShaderBytecode != nullptr ? &amplification : nullptr);
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
    created->description_ = Describe(desc, nullptr, nullptr);
    return Finish(created, riid, state, error);
}

}
