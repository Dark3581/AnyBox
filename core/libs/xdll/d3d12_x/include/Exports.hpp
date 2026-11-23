#pragma once

#include "AbxApi.hpp"

#include <windows.h>

#define D3D12CreateDevice AbxSdkD3D12CreateDevice
#define D3D12GetDebugInterface AbxSdkD3D12GetDebugInterface
#define D3D12SerializeRootSignature AbxSdkD3D12SerializeRootSignature
#define D3D12SerializeVersionedRootSignature AbxSdkD3D12SerializeVersionedRootSignature
#define D3D12CreateRootSignatureDeserializer AbxSdkD3D12CreateRootSignatureDeserializer
#define D3D12CreateVersionedRootSignatureDeserializer AbxSdkD3D12CreateVersionedRootSignatureDeserializer
#include <d3d12.h>
#undef D3D12CreateDevice
#undef D3D12GetDebugInterface
#undef D3D12SerializeRootSignature
#undef D3D12SerializeVersionedRootSignature
#undef D3D12CreateRootSignatureDeserializer
#undef D3D12CreateVersionedRootSignatureDeserializer

ABX_API HRESULT D3D12XboxCreateDevice(IUnknown* adapter, const void* parameters, REFIID riid, void** device);
ABX_API HRESULT D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minimumFeatureLevel, REFIID iid, void** device);
ABX_API HRESULT D3D12GetDebugInterface(REFIID iid, void** debug);
ABX_API HRESULT D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* rootSignature, D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob** blob, ID3DBlob** errorBlob);
ABX_API HRESULT D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* rootSignature, ID3DBlob** blob, ID3DBlob** errorBlob);
ABX_API HRESULT D3D12CreateRootSignatureDeserializer(const void* data, SIZE_T size, REFIID iid, void** deserializer);
ABX_API HRESULT D3D12CreateVersionedRootSignatureDeserializer(const void* data, SIZE_T size, REFIID iid, void** deserializer);

ABX_API HRESULT D3D12ConfigureVirtualMemoryX(const void* description);
