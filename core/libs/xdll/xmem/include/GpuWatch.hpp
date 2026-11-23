#pragma once

#include "AbxApi.hpp"

#include <cstdint>

constexpr std::uint32_t kAbxGpuMemoryWithdrawn = 0;
constexpr std::uint32_t kAbxGpuMemoryAvailable = 1;

using AbxGpuMemoryListener = void (*)(void* context, std::uint32_t event, std::uint64_t start, std::uint64_t bytes);

ABX_API void AbxXMemWatchGpuMemory(AbxGpuMemoryListener listener, void* context);
