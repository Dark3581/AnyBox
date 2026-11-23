#pragma once

#include "AbxApi.hpp"

#include <windows.h>

ABX_API HRESULT AcpHalAllocateShapeContexts();
ABX_API HRESULT AcpHalCreate();
ABX_API HRESULT AcpHalReleaseShapeContexts();
ABX_API HRESULT ApuAlloc();
ABX_API HRESULT ApuCreateHeap();
ABX_API HRESULT ApuFree();
ABX_API HRESULT ApuIsVirtualAddressValid();
ABX_API HRESULT ApuMapApuAddress();
ABX_API HRESULT ApuMapVirtualAddress();
