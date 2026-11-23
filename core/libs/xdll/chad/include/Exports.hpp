#pragma once

#include "AbxApi.hpp"

#include <windows.h>

ABX_API HRESULT XDspActivate();
ABX_API HRESULT XDspConnectWithMaximumStreamLimit();
ABX_API HRESULT XDspDeactivate();
ABX_API HRESULT XDspDisconnect();
ABX_API HRESULT XDspSubmitCommand();
