#pragma once

#include <cstddef>
#include <cstdint>

namespace abx::xmem {

constexpr std::size_t kPhysicalPageSize = 64 * 1024;

constexpr std::uint32_t kReservePlaceholder = 0x00040000;
constexpr std::uint32_t kReplacePlaceholder = 0x00004000;

void* ReservePhysicalWindow(std::size_t size, std::uint32_t protection);
void* AdoptPhysicalWindow(void* address, std::size_t size, std::uint32_t protection);
void* AllocateWithPlaceholders(void* address, std::size_t size, std::uint32_t type, std::uint32_t protection);

}
