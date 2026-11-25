#pragma once

#include "AbxApi.hpp"

#include <windows.h>

#include <cstdint>

ABX_API HRESULT XApuConnect(const void* configuration, void** shared, void** connection);
ABX_API HRESULT XApuDequeueResult();
ABX_API HRESULT XApuDisconnect(void* connection);
ABX_API HRESULT XApuEnqueueCommand(void* connection, const void* command);
ABX_API std::uint32_t XApuGetResultCount(void* connection);
