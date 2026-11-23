#pragma once

#include "AbxApi.hpp"
#include "xdll/xmem/include/Types.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>

ABX_API void* XMemAlloc(std::size_t size, std::uint64_t attributes);
ABX_API void XMemFree(void* address, std::uint64_t attributes);
ABX_API BOOL XMemVirtualQuery(HANDLE process, const void* address, XMemVirtualQueryInfo* info);
ABX_API BOOL XMemGetWorkingSetStatistics(std::uint32_t request, void* statistics);

ABX_API void* XMemAllocDefault(std::size_t size, std::uint64_t attributes);
ABX_API BOOL XMemAllocatePhysicalPages(std::uint64_t attributes, std::uintptr_t* pageCount, std::uintptr_t* pageArray);
ABX_API void XMemFreeDefault(void* address, std::uint64_t attributes);
ABX_API HRESULT XMemFreePhysicalPages();
ABX_API HRESULT XMemIsToolsMemAvailable();
ABX_API void* XMemMapPhysicalPages(void* address, std::size_t pageCount, const std::uintptr_t* pageArray);
ABX_API HRESULT XMemMapPhysicalPagesScatter();
ABX_API HRESULT XMemTransferMemory();
ABX_API void* XMemVirtualAlloc(void* address, std::size_t size, std::uint32_t allocationType, std::uint64_t attributes, std::uint32_t protect);
