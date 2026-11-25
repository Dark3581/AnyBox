#include "ConsoleFacade.hpp"

#include "AbxApi.hpp"

#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

namespace abx::d3d12x {
namespace {

constexpr GUID kUnknownInterface{0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

HRESULT STDMETHODCALLTYPE FacadeQueryInterface(Facade* self, REFIID riid, void** object) {
    if (object == nullptr) return E_POINTER;
    bool known = std::memcmp(&riid, &kUnknownInterface, sizeof(GUID)) == 0;
    for (const GUID& id : self->type->interfaces) known = known || std::memcmp(&riid, &id, sizeof(GUID)) == 0;
    if (known) {
        ++self->references;
        *object = self;
        return S_OK;
    }
    *object = nullptr;
    AbxTrace("console %s QueryInterface(%s): not served", self->type->name.c_str(), GuidText(riid).c_str());
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE FacadeAddRef(Facade* self) {
    return ++self->references;
}

ULONG STDMETHODCALLTYPE FacadeRelease(Facade* self) {
    const ULONG left = --self->references;
    if (left == 0) {
        self->inner->Release();
        delete self;
    }
    return left;
}

HRESULT STDMETHODCALLTYPE FacadeSetName(Facade* self, const WCHAR* name) {
    return static_cast<ID3D12Object*>(self->inner)->SetName(name);
}

template <std::size_t Slot>
void STDMETHODCALLTYPE Unmapped(Facade* self, const void* first, const void* second, const void* third) {
    const std::string message = "slot " + std::to_string(Slot) + " of the console " + self->type->name + " interface " + GuidText(self->type->interfaces.front()) +
                                " is not mapped: no measured call site shows which method it is; " + DescribeArguments(first, second, third);
    AbxUnsupported(("D3D12 " + self->type->name).c_str(), message.c_str());
}

template <std::size_t... Slots>
std::array<const void*, kFacadeSlots> UnmappedTable(std::index_sequence<Slots...>) {
    return {reinterpret_cast<const void*>(&Unmapped<Slots>)...};
}

std::mutex& RegistryLock() {
    static std::mutex lock;
    return lock;
}

std::vector<const FacadeType*>& Registry() {
    static std::vector<const FacadeType*> types;
    return types;
}

}

namespace {

bool Readable(const void* address, std::size_t bytes) {
    MEMORY_BASIC_INFORMATION region{};
    if (address == nullptr || VirtualQuery(address, &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT) return false;
    if ((region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
    return end - start >= bytes;
}

}

std::string DescribeArguments(const void* first, const void* second, const void* third) {
    constexpr std::size_t kWords = 16;
    std::string text = "register arguments after the object:";
    for (const void* argument : {first, second, third}) {
        char line[64];
        std::snprintf(line, sizeof(line), " %p", argument);
        text += line;
        if (!Readable(argument, kWords * sizeof(std::uint32_t))) continue;
        std::uint32_t words[kWords];
        std::memcpy(words, argument, sizeof(words));
        const void* table = nullptr;
        std::memcpy(&table, argument, sizeof(table));
        {
            const std::lock_guard guard(RegistryLock());
            for (const FacadeType* type : Registry()) {
                if (type->table.data() == table) text += " (console " + type->name + ")";
            }
        }
        text += " [";
        for (std::size_t index = 0; index < kWords; ++index) {
            std::snprintf(line, sizeof(line), index == 0 ? "%08x" : " %08x", words[index]);
            text += line;
        }
        text += "]";
    }
    return text;
}

std::string GuidText(const GUID& guid) {
    char text[40];
    std::snprintf(text, sizeof(text), "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3, guid.Data4[0],
                  guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return text;
}

FacadeType MakeFacadeType(std::string name, std::vector<GUID> interfaces) {
    FacadeType type{std::move(name), std::move(interfaces), UnmappedTable(std::make_index_sequence<kFacadeSlots>{})};
    type.table[0] = reinterpret_cast<const void*>(&FacadeQueryInterface);
    type.table[1] = reinterpret_cast<const void*>(&FacadeAddRef);
    type.table[2] = reinterpret_cast<const void*>(&FacadeRelease);
    type.table[6] = reinterpret_cast<const void*>(&FacadeSetName);
    return type;
}

void RegisterFacadeType(const FacadeType& type) {
    const std::lock_guard guard(RegistryLock());
    for (const FacadeType* known : Registry()) {
        if (known == &type) return;
    }
    Registry().push_back(&type);
}

void* WrapInFacade(const FacadeType& type, IUnknown* inner) {
    RegisterFacadeType(type);
    Facade* facade = new Facade();
    facade->table = type.table.data();
    facade->inner = inner;
    facade->type = &type;
    return facade;
}

IUnknown* FacadeInner(const void* object) {
    if (object == nullptr) return nullptr;
    const void* const* table = nullptr;
    std::memcpy(&table, object, sizeof(table));
    const std::lock_guard guard(RegistryLock());
    for (const FacadeType* type : Registry()) {
        if (type->table.data() == table) return static_cast<const Facade*>(object)->inner;
    }
    return nullptr;
}

}
