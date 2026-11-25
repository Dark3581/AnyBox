#pragma once

#include "AbxApi.hpp"

#include <windows.h>

#include <cstdint>

ABX_API HRESULT XDspActivate(void* connection, const void* second, void** state, void** stream);
ABX_API HRESULT XDspConnectWithMaximumStreamLimit(const void* first, std::uint32_t second, std::uint32_t third, std::uint32_t streamLimit, std::uint32_t fifth, void** connection);
ABX_API HRESULT XDspDeactivate(void* stream);
ABX_API HRESULT XDspDisconnect();
ABX_API HRESULT XDspSubmitCommand(void* connection, const void* command);
