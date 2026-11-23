#pragma once

#include "AbxApi.hpp"

#include <windows.h>

#include <cstdint>

ABX_API HRESULT AcpHalAllocateShapeContexts(std::uint8_t* request);
ABX_API HRESULT AcpHalCreate(void** hal);
ABX_API HRESULT AcpHalReleaseShapeContexts();
ABX_API HRESULT ApuAlloc(void** address, std::uint32_t* hardwareAddress, std::uint32_t bytes, std::uint32_t alignment, std::uint32_t heapSelector);
ABX_API HRESULT ApuCreateHeap(std::uint32_t firstBytes, std::uint32_t secondBytes);
ABX_API HRESULT ApuFree();
ABX_API HRESULT ApuIsVirtualAddressValid();
ABX_API HRESULT ApuMapApuAddress();
ABX_API HRESULT ApuMapVirtualAddress();
