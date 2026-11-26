#pragma once

#include "BindlessSet.hpp"
#include "CommandRecorder.hpp"
#include "Resource.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace abx::d3d12x {

class TextureReadback {
public:
    explicit TextureReadback(const ResourceContext& context);
    ~TextureReadback();
    TextureReadback(const TextureReadback&) = delete;
    TextureReadback& operator=(const TextureReadback&) = delete;
    static TextureReadback* Active() { return active_.load(); }

    void Consider(Resource& texture, std::uint64_t address, const D3D12_RESOURCE_DESC& desc);
    void Forget(Resource* texture);
    bool Record(Resource& texture, D3D12_RESOURCE_STATES state, VkCommandBuffer commands, std::string& error);
    void Deliver(bool present);

private:
    static constexpr std::size_t kMaxTextures = 64;
    static constexpr std::uint64_t kDefaultTexels = 1024;
    static constexpr std::uint64_t kLargestTexels = 16384;
    static constexpr std::uint64_t kRetireAfterPresents = 8;
    static constexpr std::uint64_t kRetryAfterPresents = 60;

    struct Slot {
        std::uint64_t address = 0;
        std::uint64_t texelBase = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        std::uint64_t copyBytes = 0;
        std::uint64_t sequenceOffset = 0;
        DeviceBuffer staging;
        std::vector<std::pair<std::uint64_t, std::uint32_t>> elements;
        std::uint32_t element = 0;
        std::uint32_t mode = 0;
        std::uint32_t recorded = 0;
        std::uint32_t delivered = 0;
        std::uint64_t deliveries = 0;
        std::uint64_t retryAt = 0;
        bool failureLogged = false;
        std::string name;
    };

    bool WriteTitle(std::uint64_t address, const std::uint8_t* data, std::size_t bytes, DWORD& failure) const;
    bool ReadTitle(std::uint64_t address, std::uint8_t* data, std::size_t bytes) const;
    bool DeliverLocked(Slot& slot);

    ResourceContext context_;
    std::uint64_t maxTexels_ = kDefaultTexels;
    std::mutex lock_;
    std::map<Resource*, Slot> slots_;
    std::vector<std::pair<DeviceBuffer, std::uint64_t>> retired_;
    std::vector<std::uint8_t> snapshot_;
    std::vector<std::uint8_t> run_;
    std::set<std::string> refusals_;
    std::uint64_t presents_ = 0;
    std::uint64_t considered_ = 0;
    std::uint64_t delivered_ = 0;
    std::uint64_t reported_ = 0;
    bool full_ = false;
    static inline std::atomic<TextureReadback*> active_{nullptr};
};

}
