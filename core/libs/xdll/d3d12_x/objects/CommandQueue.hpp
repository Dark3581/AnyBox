#pragma once

#include "ObjectBase.hpp"
#include "VulkanDevice.hpp"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace abx::d3d12x {

class Fence;

class CommandQueue final : public DeviceChildBase<ID3D12CommandQueue> {
public:
    static HRESULT Create(const VulkanDevice& device, IUnknown* parent, const D3D12_COMMAND_QUEUE_DESC& desc, REFIID riid, void** queue, std::string& error);

    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource*, UINT, const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*, ID3D12Heap*, UINT,
                                              const D3D12_TILE_RANGE_FLAGS*, const UINT*, const UINT*, D3D12_TILE_MAPPING_FLAGS) override;
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*, ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*,
                                            const D3D12_TILE_REGION_SIZE*, D3D12_TILE_MAPPING_FLAGS) override;
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT count, ID3D12CommandList* const* lists) override;
    void STDMETHODCALLTYPE SetMarker(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE BeginEvent(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE EndEvent() override {}
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* fence, UINT64 value) override;
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* fence, UINT64 value) override;
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* frequency) override;
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* gpu, UINT64* cpu) override;
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
    D3D12_COMMAND_QUEUE_DESC* STDMETHODCALLTYPE GetDesc(D3D12_COMMAND_QUEUE_DESC* desc) override {
        *desc = desc_;
        return desc;
    }
#else
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override { return desc_; }
#endif

    void Flush();
    std::size_t Pending() const;

protected:
    bool Implements(REFIID riid) const override;

private:
    struct Operation {
        bool wait = false;
        Fence* fence = nullptr;
        UINT64 value = 0;
    };

    CommandQueue(const VulkanDevice& device, IUnknown* parent, const D3D12_COMMAND_QUEUE_DESC& desc) : DeviceChildBase(parent), device_(device), desc_(desc) {}
    ~CommandQueue() override;
    HRESULT Enqueue(Operation operation);
    bool Submit(const Operation& operation);

    const VulkanDevice& device_;
    D3D12_COMMAND_QUEUE_DESC desc_;
    mutable std::mutex lock_;
    std::deque<Operation> pending_;
    bool flushing_ = false;
    bool again_ = false;
};

}
