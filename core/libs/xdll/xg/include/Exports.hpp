#pragma once

#include "AbxApi.hpp"

#include <windows.h>
#include <d3d12.h>

#include <cstdint>

ABX_API HRESULT XGCreateTextureComputer(const D3D12_RESOURCE_DESC* desc, void** computer);
ABX_API HRESULT XGSuggestSwizzleMode(const void* desc, std::uint32_t flags, std::uint32_t* layout);
