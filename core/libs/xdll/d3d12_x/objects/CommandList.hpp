#pragma once

#include "CommandAllocator.hpp"
#include "CommandRecorder.hpp"
#include "DescriptorHeap.hpp"
#include "PipelineObject.hpp"
#include "RootSignature.hpp"
#include "ObjectBase.hpp"
#include "Resource.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace abx::d3d12x {

struct PendingConstants {
    UINT index = 0;
    UINT offset = 0;
    std::vector<std::uint32_t> values;
};

struct RootArguments {
    const RootSignature* signature = nullptr;
    std::array<std::uint32_t, kRootSignatureWordLimit> words{};
    std::vector<PendingConstants> pending;
};

class CommandList final : public DeviceChildBase<ID3D12GraphicsCommandList> {
public:
    static HRESULT Create(const ResourceContext& context, IUnknown* parent, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial,
                          REFIID riid, void** list, std::string& error);

    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return type_; }
    HRESULT STDMETHODCALLTYPE Close() override;
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial) override;
    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState* state) override;
    void STDMETHODCALLTYPE DrawInstanced(UINT, UINT, UINT, UINT) override;
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT, UINT, UINT, INT, UINT) override;
    void STDMETHODCALLTYPE Dispatch(UINT, UINT, UINT) override;
    void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource* destination, UINT64 destinationOffset, ID3D12Resource* source, UINT64 sourceOffset, UINT64 bytes) override;
    void CopyMemoryToMemory(std::uint64_t destination, std::uint64_t source, std::uint64_t bytes, const char* function);
    void DrawIndexedAt(std::uint64_t address, DXGI_FORMAT format, UINT count, const char* function);
    void* AllocateTransient(std::size_t bytes, std::size_t alignment, const char* function);
    void FillTitleMemory(std::uint64_t destination, std::uint64_t bytes, const std::uint8_t (&pattern)[16], std::uint32_t unit, const char* function);
    void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* destination, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* source,
                                             const D3D12_BOX* box) override;
    void STDMETHODCALLTYPE CopyResource(ID3D12Resource* destination, ID3D12Resource* source) override;
    void STDMETHODCALLTYPE CopyTiles(ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*, ID3D12Resource*, UINT64, D3D12_TILE_COPY_FLAGS) override;
    void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource* destination, UINT destinationSubresource, ID3D12Resource* source, UINT sourceSubresource, DXGI_FORMAT format) override;
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology) override;
    void STDMETHODCALLTYPE RSSetViewports(UINT count, const D3D12_VIEWPORT* viewports) override;
    void STDMETHODCALLTYPE RSSetScissorRects(UINT count, const D3D12_RECT* rects) override;
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT factor[4]) override;
    void STDMETHODCALLTYPE OMSetStencilRef(UINT reference) override;
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState*) override;
    void STDMETHODCALLTYPE ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers) override;
    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList*) override;
    void STDMETHODCALLTYPE SetDescriptorHeaps(UINT, ID3D12DescriptorHeap* const*) override;
    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature*) override;
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature*) override;
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT, D3D12_GPU_DESCRIPTOR_HANDLE) override;
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT, D3D12_GPU_DESCRIPTOR_HANDLE) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT, UINT, UINT) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT, UINT, UINT) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT, UINT, const void*, UINT) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT, UINT, const void*, UINT) override;
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view) override;
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW* views) override;
    void STDMETHODCALLTYPE SOSetTargets(UINT, UINT, const D3D12_STREAM_OUTPUT_BUFFER_VIEW*) override;
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*) override;
    void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*) override;
    void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT[4], UINT, const D3D12_RECT*) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*, const UINT[4], UINT, const D3D12_RECT*) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*, const float[4], UINT, const D3D12_RECT*) override;
    void STDMETHODCALLTYPE DiscardResource(ID3D12Resource*, const D3D12_DISCARD_REGION*) override {}
    void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT) override;
    void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT) override;
    void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT, UINT, ID3D12Resource*, UINT64) override;
    void STDMETHODCALLTYPE SetPredication(ID3D12Resource*, UINT64, D3D12_PREDICATION_OP) override;
    void STDMETHODCALLTYPE SetMarker(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE BeginEvent(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE EndEvent() override {}
    void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64) override;

    VkCommandBuffer Commands() const { return commands_; }
    bool Closed() const { return closed_; }
    bool CopyPending() const { return copied_; }
    DescriptorHeap* ShaderResourceHeap() const { return shaderResourceHeap_; }
    DescriptorHeap* SamplerHeap() const { return samplerHeap_; }
    const std::vector<AttachmentRef>& RenderTargets() const { return renderTargets_; }
    const std::optional<AttachmentRef>& DepthStencil() const { return depthStencil_; }
    const PipelineStateObject* Pipeline() const { return pipeline_; }
    void DispatchMesh(UINT x, UINT y, UINT z);
    void DispatchThreads(UINT x, UINT y, UINT z, UINT flags);
    const RootArguments& GraphicsArguments() const { return graphics_; }
    const RootArguments& ComputeArguments() const { return compute_; }

protected:
    bool Implements(REFIID riid) const override;

private:
    CommandList(const ResourceContext& context, IUnknown* parent, D3D12_COMMAND_LIST_TYPE type) : DeviceChildBase(parent), context_(context), type_(type) {}
    ~CommandList() override { alive_ = 0; }
    void BeforeDelete() override;
    void ProtectHead(bool readOnly);

public:
    static void* operator new(std::size_t size);
    static void operator delete(void* object);

private:
    void ReportDamage(const char* function) const;
    void ClearUnorderedAccess(ID3D12Resource* resource, const VkClearColorValue& value, UINT count, const char* function);

public:
    bool Alive() const { return alive_ == kAlive; }
    std::uint32_t AliveTag() const { return alive_; }

private:
    static constexpr std::uint32_t kAlive = 0x4C495354u;
    std::uint32_t alive_ = kAlive;
    bool Begin(ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial, std::string& error);
    Resource* Ours(ID3D12Resource* resource, const char* function) const;
    D3D12_RESOURCE_STATES StateOf(const Resource* resource, UINT subresource, D3D12_RESOURCE_STATES expected) const;
    void Check(bool ok, const char* function, const std::string& error) const;
    CommandRecorder Recorder(const char* function) const;
    BufferRef WindowRange(D3D12_GPU_VIRTUAL_ADDRESS address, UINT64 bytes, const char* function) const;
    const DescriptorRecord& Attachment(D3D12_CPU_DESCRIPTOR_HANDLE handle, D3D12_DESCRIPTOR_HEAP_TYPE type, const char* function) const;
    void ResetBindings();
    void OrderCopy(const char* function);
    std::optional<TitleAddressSpace::Location> LocateTitleMemory(std::uint64_t address, std::uint64_t wanted, bool write, std::uint64_t& available, std::uint64_t bytes, const char* function);
    void BindPipeline(ID3D12PipelineState* state, const char* function);
    void SetSignature(RootArguments& arguments, ID3D12RootSignature* signature, const char* function);
    void ReportRootState(const RootArguments& arguments, const char* function) const;
    void EndPass();
    bool PrepareDraw(const char* function);
    bool SkippedWork(const char* function);
    bool ResolveCodeBase(const char* function, std::string& reason);
    void PushUserData(RootArguments& arguments, std::uint32_t firstSgpr, const char* function, std::uint32_t threadLimit = 0);
    void BindTitleSet(VkPipelineBindPoint point);
    const RootParameterLayout& Parameter(const RootArguments& arguments, UINT index, D3D12_ROOT_PARAMETER_TYPE type, const char* function) const;
    void SetConstants(RootArguments& arguments, UINT index, UINT count, const void* data, UINT offset, const char* function);
    static bool WriteConstants(RootArguments& arguments, UINT index, UINT offset, const std::uint32_t* values, UINT count);
    static void ApplyPending(RootArguments& arguments);
    void SetRootView(RootArguments& arguments, UINT index, D3D12_ROOT_PARAMETER_TYPE type, D3D12_GPU_VIRTUAL_ADDRESS address, const char* function);
    void SetTable(RootArguments& arguments, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle, const char* function);

    ResourceContext context_;
    D3D12_COMMAND_LIST_TYPE type_;
    VkCommandBuffer commands_ = VK_NULL_HANDLE;
    std::uint32_t profileGeneration_ = 0;
    std::uint64_t profileList_ = 0;
    bool closed_ = true;
    bool copied_ = false;
    std::map<std::pair<const Resource*, UINT>, D3D12_RESOURCE_STATES> states_;
    DescriptorHeap* shaderResourceHeap_ = nullptr;
    DescriptorHeap* samplerHeap_ = nullptr;
    std::vector<AttachmentRef> renderTargets_;
    std::vector<Resource*> targetResources_;
    std::vector<Resource*> readbackWritten_;
    void RecordReadback(Resource* resource, D3D12_RESOURCE_STATES state, const char* function);
    void RecordDepthShadow(Resource* resource, D3D12_RESOURCE_STATES state, bool enters, const char* function);
    std::array<D3D12_VERTEX_BUFFER_VIEW, 4> traceViews_{};
    D3D12_INDEX_BUFFER_VIEW indexView_{};
    CommandAllocator* allocator_ = nullptr;
    std::optional<AttachmentRef> depthStencil_;
    Resource* depthResource_ = nullptr;
    bool targetsSet_ = false;
    UINT emptyTargets_ = 0;
    bool depthHandleGiven_ = false;
    class HeldPipeline {
    public:
        HeldPipeline() = default;
        ~HeldPipeline() { Reset(); }
        HeldPipeline(const HeldPipeline&) = delete;
        HeldPipeline& operator=(const HeldPipeline&) = delete;
        HeldPipeline& operator=(PipelineStateObject* pipeline) {
            if (pipeline != nullptr) pipeline->AddRef();
            Reset();
            pipeline_ = pipeline;
            return *this;
        }
        HeldPipeline& operator=(std::nullptr_t) {
            Reset();
            return *this;
        }
        PipelineStateObject* operator->() const { return pipeline_; }
        operator PipelineStateObject*() const { return pipeline_; }

    private:
        void Reset() {
            if (pipeline_ != nullptr) pipeline_->Release();
            pipeline_ = nullptr;
        }
        PipelineStateObject* pipeline_ = nullptr;
    };
    HeldPipeline pipeline_;
    RootArguments graphics_;
    RootArguments compute_;
    bool rendering_ = false;
    bool rectangles_ = false;
    std::vector<D3D12_VIEWPORT> viewports_;
    std::vector<D3D12_RECT> scissors_;
    std::optional<D3D12_PRIMITIVE_TOPOLOGY> topology_;
    const char* checkpointDetail_ = nullptr;
    bool pipelineBound_ = false;
    bool graphicsSetBound_ = false;
    bool computeSetBound_ = false;
};

}
