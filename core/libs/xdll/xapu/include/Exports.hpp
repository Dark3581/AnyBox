#pragma once

#include "AbxApi.hpp"

#include <windows.h>

ABX_API HRESULT XApuConnect();
ABX_API HRESULT XApuDequeueResult();
ABX_API HRESULT XApuDisconnect();
ABX_API HRESULT XApuEnqueueCommand();
ABX_API HRESULT XApuGetResultCount();
