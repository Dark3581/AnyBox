#include "ConsoleFacade.hpp"

#include "AbxApi.hpp"

#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <utility>

namespace abx::d3d12x {
namespace {

constexpr GUID kUnknownInterface{0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

HRESULT STDMETHODCALLTYPE FacadeQueryInterface(Facade* self, REFIID riid, void** object) {
    if (object == nullptr) return E_POINTER;
    bool known = std::memcmp(&riid, &kUnknownInterface, sizeof(GUID)) == 0;
    for (const GUID& id : Header(self).type->interfaces) known = known || std::memcmp(&riid, &id, sizeof(GUID)) == 0;
    if (known) {
        ++Header(self).references;
        *object = self;
        return S_OK;
    }
    *object = nullptr;
    AbxTrace("console %s QueryInterface(%s): not served", Header(self).type->name.c_str(), GuidText(riid).c_str());
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE FacadeAddRef(Facade* self) {
    return ++Header(self).references;
}

ULONG STDMETHODCALLTYPE FacadeRelease(Facade* self) {
    FacadeHeader& header = Header(self);
    const ULONG left = --header.references;
    if (left == 0) {
        header.inner->Release();
        header.~FacadeHeader();
        ::operator delete(&header);
    }
    return left;
}

HRESULT STDMETHODCALLTYPE FacadeSetName(Facade* self, const WCHAR* name) {
    return static_cast<ID3D12Object*>(Header(self).inner)->SetName(name);
}

template <std::size_t Slot>
void STDMETHODCALLTYPE Unmapped(Facade* self, const void* first, const void* second, const void* third) {
    const std::string message = "slot " + std::to_string(Slot) + " of the console " + Header(self).type->name + " interface " + GuidText(Header(self).type->interfaces.front()) +
                                " is not mapped: no measured call site shows which method it is; " + DescribeArguments(first, second, third);
    AbxUnsupported(("D3D12 " + Header(self).type->name).c_str(), message.c_str());
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
    const std::size_t total = sizeof(FacadeHeader) + sizeof(Facade) + type.titleBytes + kFacadeGuardBytes;
    auto* block = static_cast<unsigned char*>(::operator new(total));
    std::memset(block, 0, total);
    FacadeHeader* header = new (block) FacadeHeader();
    header->inner = inner;
    header->type = &type;
    header->titleBytes = type.titleBytes;
    header->innerCopy = inner;
    header->typeCopy = &type;
    header->innerHead = ObjectHead(inner);
    auto* facade = reinterpret_cast<Facade*>(block + sizeof(FacadeHeader));
    facade->table = type.table.data();
    std::memset(reinterpret_cast<unsigned char*>(facade) + sizeof(Facade) + type.titleBytes, kFacadeGuardByte, kFacadeGuardBytes);
    return facade;
}

void FacadeDamaged(const Facade* facade, const char* where) {
    const FacadeHeader& header = Header(facade);
    const auto* guard = reinterpret_cast<const unsigned char*>(facade) + sizeof(Facade) + header.titleBytes;
    std::string dump;
    char text[160];
    for (std::size_t at = 0; at < kFacadeGuardBytes; at += 8) {
        std::uint64_t word = 0;
        std::memcpy(&word, guard + at, sizeof(word));
        if (word == 0xA5A5A5A5A5A5A5A5ull) continue;
        std::snprintf(text, sizeof(text), " +0x%zx=%016llx", sizeof(Facade) + header.titleBytes + at, static_cast<unsigned long long>(word));
        dump += text;
    }
    std::snprintf(text, sizeof(text), "object %p (title area 0x%zx bytes), inner %p/%p, type %p/%p; changed guard words:", static_cast<const void*>(facade), header.titleBytes,
                  static_cast<const void*>(header.inner), static_cast<const void*>(header.innerCopy), static_cast<const void*>(header.type),
                  static_cast<const void*>(header.typeCopy));
    AbxUnsupported(where, ("a console object written past its title area, or whose AnyBox fields in front of it changed: " + std::string(text) + dump).c_str());
}

std::size_t HighestTitleByte(const Facade* facade) {
    const FacadeHeader& header = Header(facade);
    const auto* area = reinterpret_cast<const unsigned char*>(facade) + sizeof(Facade);
    for (std::size_t at = header.titleBytes; at > 0; --at) {
        if (area[at - 1] != 0) return sizeof(Facade) + at;
    }
    return 0;
}

std::uint64_t ObjectHead(const void* object) {
    std::uint64_t head = 0;
    if (object != nullptr) std::memcpy(&head, object, sizeof(head));
    return head;
}

void InnerChanged(const Facade* facade, const void* object, const char* what) {
    const auto* bytes = static_cast<const unsigned char*>(object);
    std::string dump;
    char text[96];
    for (std::size_t at = 0; at < 0x100; at += 8) {
        std::uint64_t word = 0;
        std::memcpy(&word, bytes + at, sizeof(word));
        std::snprintf(text, sizeof(text), " +0x%zx=%016llx", at, static_cast<unsigned long long>(word));
        dump += text;
    }
    std::snprintf(text, sizeof(text), "facade %p, object %p, first 8 bytes were %016llx;", static_cast<const void*>(facade), object,
                  static_cast<unsigned long long>(Header(facade).innerHead));
    AbxUnsupported("console object call", ("the AnyBox object behind a console object was damaged: " + std::string(what) + "; " + std::string(text) + " first words:" + dump).c_str());
}

void InnerDead(const Facade* facade, const void* object, std::uint32_t tag) {
    char text[64];
    std::snprintf(text, sizeof(text), "its alive tag reads 0x%08x (freed, or overwritten)", tag);
    InnerChanged(facade, object, text);
}

void CheckFacadeGuard(const Facade* facade, const char* where) {
    const FacadeHeader& header = Header(facade);
    if (header.inner != header.innerCopy || header.type != header.typeCopy) FacadeDamaged(facade, where);
    const auto* guard = reinterpret_cast<const unsigned char*>(facade) + sizeof(Facade) + header.titleBytes;
    for (std::size_t at = 0; at < kFacadeGuardBytes; ++at) {
        if (guard[at] != kFacadeGuardByte) FacadeDamaged(facade, where);
    }
    static std::atomic<std::size_t> highest{0};
    const std::size_t used = HighestTitleByte(facade);
    std::size_t seen = highest.load();
    while (used > seen && !highest.compare_exchange_weak(seen, used)) {
    }
    if (used > seen) {
        AbxLog("d3d12_x", "DIAG the title has written console %s fields up to byte 0x%zx of its object (AnyBox gives it 0x%zx bytes)", header.type->name.c_str(), used,
               sizeof(Facade) + header.titleBytes);
    }
}

IUnknown* FacadeInner(const void* object) {
    if (object == nullptr) return nullptr;
    const void* const* table = nullptr;
    std::memcpy(&table, object, sizeof(table));
    const std::lock_guard guard(RegistryLock());
    for (const FacadeType* type : Registry()) {
        if (type->table.data() == table) {
            const Facade* facade = static_cast<const Facade*>(object);
            const FacadeHeader& header = Header(facade);
            if (header.inner != header.innerCopy || header.type != header.typeCopy) FacadeDamaged(facade, "a console object passed to a call");
            if (ObjectHead(header.inner) != header.innerHead) InnerChanged(facade, header.inner, "its first 8 bytes (the vtable pointer) changed");
            return header.inner;
        }
    }
    return nullptr;
}

}
