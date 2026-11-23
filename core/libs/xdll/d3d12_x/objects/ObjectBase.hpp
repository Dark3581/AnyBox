#pragma once

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace abx::d3d12x {

struct GuidLess {
    bool operator()(const GUID& a, const GUID& b) const { return std::memcmp(&a, &b, sizeof(GUID)) < 0; }
};

class PrivateData {
public:
    PrivateData() = default;
    PrivateData(const PrivateData&) = delete;
    PrivateData& operator=(const PrivateData&) = delete;
    ~PrivateData() {
        for (auto& [guid, entry] : entries_) {
            if (entry.object != nullptr) entry.object->Release();
        }
    }

    HRESULT Get(REFGUID guid, UINT* size, void* data) {
        if (size == nullptr) return E_INVALIDARG;
        std::lock_guard<std::mutex> guard(lock_);
        const auto found = entries_.find(guid);
        if (found == entries_.end()) {
            *size = 0;
            return DXGI_ERROR_NOT_FOUND;
        }
        const UINT needed = found->second.object != nullptr ? static_cast<UINT>(sizeof(IUnknown*)) : static_cast<UINT>(found->second.bytes.size());
        if (data == nullptr) {
            *size = needed;
            return S_OK;
        }
        if (*size < needed) {
            *size = needed;
            return DXGI_ERROR_MORE_DATA;
        }
        *size = needed;
        if (found->second.object != nullptr) {
            found->second.object->AddRef();
            std::memcpy(data, &found->second.object, sizeof(IUnknown*));
        } else if (needed != 0) {
            std::memcpy(data, found->second.bytes.data(), needed);
        }
        return S_OK;
    }

    HRESULT Set(REFGUID guid, UINT size, const void* data) {
        std::lock_guard<std::mutex> guard(lock_);
        Remove(guid);
        if (data == nullptr) return S_OK;
        Entry entry;
        entry.bytes.assign(static_cast<const unsigned char*>(data), static_cast<const unsigned char*>(data) + size);
        entries_.emplace(guid, std::move(entry));
        return S_OK;
    }

    HRESULT SetInterface(REFGUID guid, const IUnknown* object) {
        std::lock_guard<std::mutex> guard(lock_);
        Remove(guid);
        if (object == nullptr) return S_OK;
        Entry entry;
        entry.object = const_cast<IUnknown*>(object);
        entry.object->AddRef();
        entries_.emplace(guid, std::move(entry));
        return S_OK;
    }

private:
    struct Entry {
        std::vector<unsigned char> bytes;
        IUnknown* object = nullptr;
    };

    void Remove(REFGUID guid) {
        const auto found = entries_.find(guid);
        if (found == entries_.end()) return;
        if (found->second.object != nullptr) found->second.object->Release();
        entries_.erase(found);
    }

    std::mutex lock_;
    std::map<GUID, Entry, GuidLess> entries_;
};

template <typename Interface>
class DeviceChildBase : public Interface {
public:
    DeviceChildBase(const DeviceChildBase&) = delete;
    DeviceChildBase& operator=(const DeviceChildBase&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (!Implements(riid)) return E_NOINTERFACE;
        this->AddRef();
        *object = static_cast<Interface*>(this);
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references_;
        if (left == 0) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override { return privateData_.Get(guid, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override { return privateData_.Set(guid, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override { return privateData_.SetInterface(guid, data); }
    HRESULT STDMETHODCALLTYPE SetName(const WCHAR* name) override {
        std::lock_guard<std::mutex> guard(nameLock_);
        name_ = name != nullptr ? name : L"";
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** device) override {
        if (device == nullptr) return E_POINTER;
        *device = nullptr;
        return parent_ != nullptr ? parent_->QueryInterface(riid, device) : E_NOINTERFACE;
    }

    std::wstring Name() const {
        std::lock_guard<std::mutex> guard(nameLock_);
        return name_;
    }

protected:
    explicit DeviceChildBase(IUnknown* parent) : parent_(parent) {
        if (parent_ != nullptr) parent_->AddRef();
    }
    virtual ~DeviceChildBase() {
        if (parent_ != nullptr) parent_->Release();
    }
    virtual bool Implements(REFIID riid) const {
        return riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild);
    }

private:
    std::atomic<ULONG> references_{1};
    IUnknown* parent_;
    PrivateData privateData_;
    mutable std::mutex nameLock_;
    std::wstring name_;
};

}
