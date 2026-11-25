#pragma once

#include "xdll/gameinput/Gamepad.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace abx::gameinput {

inline constexpr HRESULT kDeviceNotFound = static_cast<HRESULT>(0x838A0002u);
inline constexpr HRESULT kReadingNotFound = static_cast<HRESULT>(0x838A0003u);
inline constexpr std::uint64_t kUnimplementedResult = 0x80004001ull;

inline constexpr GUID kIidIUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
inline constexpr GUID kIidIGameInput = {0x11be2a7e, 0x4254, 0x445a, {0x9c, 0x09, 0xff, 0xc4, 0x0f, 0x00, 0x69, 0x18}};
inline constexpr GUID kIidIGameInputReading = {0x2156947a, 0xe1fa, 0x4de0, {0xa3, 0x0b, 0xd8, 0x12, 0x93, 0x1d, 0xbd, 0x8d}};
inline constexpr GUID kIidIGameInputDevice = {0x31dd86fb, 0x4c1b, 0x408a, {0x86, 0x8f, 0x43, 0x9b, 0x3c, 0xd4, 0x71, 0x25}};

inline constexpr std::size_t kInputSlots = 24;
inline constexpr std::size_t kDeviceSlots = 24;
inline constexpr std::size_t kReadingSlots = 32;

struct DeviceInfo {
    std::uint32_t infoSize;
    std::uint16_t vendorId;
    std::uint16_t productId;
    std::uint16_t revisionNumber;
    std::uint8_t interfaceNumber;
    std::uint8_t collectionNumber;
    std::uint16_t usagePage;
    std::uint16_t usageId;
    std::array<std::uint16_t, 4> hardwareVersion;
    std::array<std::uint16_t, 4> firmwareVersion;
    std::array<std::uint8_t, 32> deviceId;
    std::array<std::uint8_t, 32> deviceRootId;
    std::int32_t deviceFamily;
    std::uint32_t capabilities;
    std::uint32_t supportedInput;
    std::uint32_t supportedRumbleMotors;
    std::array<std::uint8_t, 896> rest;
};

static_assert(offsetof(DeviceInfo, deviceId) == 32);
static_assert(offsetof(DeviceInfo, deviceFamily) == 96);
static_assert(offsetof(DeviceInfo, supportedInput) == 104);
static_assert(sizeof(DeviceInfo) == 1008);

inline constexpr std::array<std::uint8_t, 32> kDeviceId = {'A', 'n', 'y', 'B', 'o', 'x', ' ', 'g', 'a', 'm', 'e', 'p', 'a', 'd', ' ', '0',
                                                           ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};

struct Reading {
    const void* const* vtable;
    std::atomic<std::uint32_t> references;
    GamepadState state;
    std::uint64_t sequence;
    std::uint64_t timestamp;
};

void* InputObject();
void* DeviceObject();
const DeviceInfo& GamepadDeviceInfo();
Reading* NewReading(const GamepadState& state);
void DispatchReadingCallbacks(const GamepadState& state);
HRESULT Create(void** gameInput);

}
