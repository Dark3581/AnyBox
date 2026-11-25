#pragma once

#include "AbxApi.hpp"

#include <windows.h>

#include <cstdint>

ABX_API HRESULT AcpHalAllocateShapeContexts(std::uint8_t* request);
ABX_API HRESULT AcpHalCreate(void** hal);
ABX_API HRESULT AcpHalReleaseShapeContexts();
ABX_API bool ApuIsVirtualAddressValid(const void* address, std::uint32_t bytes);
ABX_API void* ApuMapApuAddress(std::uint32_t apuAddress);
ABX_API std::uint32_t ApuMapVirtualAddress(const void* address);
ABX_API HRESULT ApuAlloc(void** address, std::uint32_t* hardwareAddress, std::uint32_t bytes, std::uint32_t alignment, std::uint32_t heapSelector);
ABX_API HRESULT ApuCreateHeap(std::uint32_t firstBytes, std::uint32_t secondBytes);
ABX_API HRESULT ApuFree();
