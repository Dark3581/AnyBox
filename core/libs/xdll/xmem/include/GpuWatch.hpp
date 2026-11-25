#pragma once

#include "AbxApi.hpp"

#include <cstdint>

constexpr std::uint32_t kAbxGpuMemoryWithdrawn = 0;
constexpr std::uint32_t kAbxGpuMemoryAvailable = 1;
constexpr std::uint32_t kAbxGpuMemoryBackingAvailable = 2;
constexpr std::uint32_t kAbxGpuMemoryBackingWithdrawn = 3;
constexpr std::uint32_t kAbxGpuMemoryAliased = 4;
constexpr std::uint32_t kAbxGpuMemoryWritten = 5;

using AbxGpuMemoryListener = void (*)(void* context, std::uint32_t event, std::uint64_t start, std::uint64_t bytes, std::uint64_t source);

ABX_API void AbxXMemWatchGpuMemory(AbxGpuMemoryListener listener, void* context);
ABX_API void AbxXMemAnnounceWritten(std::uint64_t start, std::uint64_t bytes);
ABX_API void* AbxXMemPhysicalPageView(std::uintptr_t frame);
ABX_API void* AbxXMemPhysicalViewAt(std::uintptr_t address);
ABX_API BOOL AbxXMemMirrorPhysicalPage(std::uintptr_t address, std::uintptr_t source);
ABX_API BOOL AbxXMemMapFrameAt(std::uintptr_t address, std::uintptr_t frame);
