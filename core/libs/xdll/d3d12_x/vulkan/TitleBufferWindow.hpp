#pragma once

#include "VulkanDevice.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace abx::d3d12x {

constexpr std::uint64_t kLowWindowStart = 0x10000000ull;
constexpr std::uint64_t kLowWindowEnd = 0x100000000ull;

std::optional<std::uint64_t> FindFreeRange(std::uint64_t bytes, std::uint64_t alignment, std::uint64_t start, std::uint64_t end);

class TitleBufferWindow {
public:
    static std::unique_ptr<TitleBufferWindow> Create(const VulkanDevice& device, std::uint64_t bytes, std::string& error);
    ~TitleBufferWindow();
    TitleBufferWindow(const TitleBufferWindow&) = delete;
    TitleBufferWindow& operator=(const TitleBufferWindow&) = delete;

    std::uint8_t* Base() const { return base_; }
    std::uint64_t Size() const { return size_; }
    std::uint64_t Start() const { return reinterpret_cast<std::uint64_t>(base_); }
    VkBuffer Buffer() const { return buffer_; }
    VkDeviceAddress DeviceAddress() const { return address_; }
    std::uint64_t HeapBase() const { return address_ - Start(); }
    bool Contains(std::uint64_t address, std::uint64_t bytes) const;

private:
    explicit TitleBufferWindow(const VulkanDevice& device) : device_(device) {}
    bool Initialize(std::uint64_t bytes, std::string& error);

    const VulkanDevice& device_;
    std::uint8_t* base_ = nullptr;
    std::uint64_t size_ = 0;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VkDeviceAddress address_ = 0;
};

}
