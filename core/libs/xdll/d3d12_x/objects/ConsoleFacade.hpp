#pragma once

#include <windows.h>
#include <unknwn.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace abx::d3d12x {

constexpr std::size_t kFacadeSlots = 256;
constexpr std::size_t kFacadeTitleFieldBytes = 0x100;
constexpr std::size_t kFacadeGuardBytes = 0x200;

struct FacadeType;

struct alignas(16) FacadeHeader {
    IUnknown* inner = nullptr;
    const FacadeType* type = nullptr;
    std::atomic<ULONG> references{1};
    std::size_t titleBytes = 0;
    IUnknown* innerCopy = nullptr;
    const FacadeType* typeCopy = nullptr;
    std::uint64_t innerHead = 0;
};

struct Facade {
    const void* const* table;
};

inline FacadeHeader& Header(Facade* facade) {
    return *reinterpret_cast<FacadeHeader*>(reinterpret_cast<unsigned char*>(facade) - sizeof(FacadeHeader));
}

inline const FacadeHeader& Header(const Facade* facade) {
    return *reinterpret_cast<const FacadeHeader*>(reinterpret_cast<const unsigned char*>(facade) - sizeof(FacadeHeader));
}

constexpr unsigned char kFacadeGuardByte = 0xA5;

struct FacadeType {
    std::string name;
    std::vector<GUID> interfaces;
    std::array<const void*, kFacadeSlots> table;
    std::size_t titleBytes = kFacadeTitleFieldBytes;
};

std::string GuidText(const GUID& guid);
std::string DescribeArguments(const void* first, const void* second, const void* third);
FacadeType MakeFacadeType(std::string name, std::vector<GUID> interfaces);
void RegisterFacadeType(const FacadeType& type);
void* WrapInFacade(const FacadeType& type, IUnknown* inner);
[[noreturn]] void FacadeDamaged(const Facade* facade, const char* where);
void CheckFacadeGuard(const Facade* facade, const char* where);
[[noreturn]] void InnerDead(const Facade* facade, const void* object, std::uint32_t tag);
[[noreturn]] void InnerChanged(const Facade* facade, const void* object, const char* what);
std::uint64_t ObjectHead(const void* object);
IUnknown* FacadeInner(const void* object);

template <typename Object>
Object* InnerOf(Facade* facade) {
    const FacadeHeader& header = Header(facade);
    if (header.inner != header.innerCopy || header.type != header.typeCopy) FacadeDamaged(facade, "a call through the object");
    Object* object = static_cast<Object*>(header.inner);
    if (ObjectHead(object) != header.innerHead) InnerChanged(facade, object, "its first 8 bytes (the vtable pointer) changed");
    if constexpr (requires(const Object& candidate) { candidate.AliveTag(); }) {
        if (!object->Alive()) InnerDead(facade, object, object->AliveTag());
    }
    return object;
}

}
