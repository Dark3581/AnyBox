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

constexpr std::uint32_t kAbxPhysicalPageGuardRefused = 0xFFFFFFFFu;
constexpr BOOL kAbxPhysicalPageGuardAll = 1;
constexpr BOOL kAbxPhysicalPageGuardWrites = 2;

using AbxPhysicalPageReturn = void (*)(void* context, std::uint64_t source);

ABX_API void AbxXMemTrackPhysicalViews(AbxPhysicalPageReturn returned, void* context);
ABX_API std::uint32_t AbxXMemGuardPhysicalPage(std::uint64_t source, BOOL guard, std::uint64_t* views, std::uint32_t capacity);
ABX_API BOOL AbxXMemCopyToPhysicalPage(std::uintptr_t frame, std::size_t offset, const void* data, std::size_t bytes);
