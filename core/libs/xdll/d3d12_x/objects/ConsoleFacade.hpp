#pragma once

#include <windows.h>
#include <unknwn.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace abx::d3d12x {

constexpr std::size_t kFacadeSlots = 256;
constexpr std::size_t kFacadeTitleFieldBytes = 0x100;

struct FacadeType;

struct Facade {
    const void* const* table;
    std::array<unsigned char, kFacadeTitleFieldBytes> titleFields{};
    IUnknown* inner;
    const FacadeType* type;
    std::atomic<ULONG> references{1};
};

struct FacadeType {
    std::string name;
    std::vector<GUID> interfaces;
    std::array<const void*, kFacadeSlots> table;
};

std::string GuidText(const GUID& guid);
std::string DescribeArguments(const void* first, const void* second, const void* third);
FacadeType MakeFacadeType(std::string name, std::vector<GUID> interfaces);
void RegisterFacadeType(const FacadeType& type);
void* WrapInFacade(const FacadeType& type, IUnknown* inner);
IUnknown* FacadeInner(const void* object);

template <typename Object>
Object* InnerOf(Facade* facade) {
    return static_cast<Object*>(facade->inner);
}

}
