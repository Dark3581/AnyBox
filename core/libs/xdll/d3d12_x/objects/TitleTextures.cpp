#include "TitleTextures.hpp"

#include "AbxApi.hpp"
#include "BufferArena.hpp"
#include "CopyLayout.hpp"
#include "TextureDesc.hpp"
#include "TitleAddressSpace.hpp"
#include "TitleBufferWindow.hpp"

#include <cstring>
#include <optional>

namespace abx::d3d12x {

namespace {

constexpr D3D12_RESOURCE_STATES kShaderRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

bool Readable(std::uint64_t address, std::uint64_t bytes) {
    std::uint64_t at = address;
    while (at < address + bytes) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            return false;
        }
        at = reinterpret_cast<std::uint64_t>(region.BaseAddress) + region.RegionSize;
    }
    return true;
}

}

TitleTextures::~TitleTextures() {
    for (auto& [key, texture] : textures_) texture->Release();
}

Resource* TitleTextures::At(std::uint64_t address, const D3D12_RESOURCE_DESC& desc, std::string& error) {
    D3D12_RESOURCE_DESC applied = desc;
    applied.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    applied.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<UINT>(applied.Flags) & 0xFFFFu);
    applied.Alignment = 0;
    std::vector<std::uint8_t> key(sizeof(applied));
    std::memcpy(key.data(), &applied, sizeof(applied));
    const std::lock_guard guard(lock_);
    const auto known = textures_.find({address, key});
    if (known != textures_.end()) {
        const auto waiting = pending_.find(known->second);
        if (waiting != pending_.end()) {
            std::string retry;
            const Filled filled = Upload(*known->second, waiting->second.first, waiting->second.second, retry);
            if (filled == Filled::Yes) pending_.erase(waiting);
        }
        return known->second;
    }
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    Resource* texture = nullptr;
    if (FAILED(Resource::CreateCommitted(context_, parent_, heap, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, applied, D3D12_RESOURCE_STATE_COPY_DEST, __uuidof(ID3D12Resource),
                                         reinterpret_cast<void**>(&texture), error))) {
        return nullptr;
    }
    const std::uint32_t mips = applied.MipLevels != 0 ? applied.MipLevels : FullMipChain(applied.Width, applied.Height, applied.DepthOrArraySize);
    const Filled filled = Upload(*texture, address, mips, error);
    if (filled == Filled::Failed) {
        texture->Release();
        return nullptr;
    }
    if (filled == Filled::NotMapped) {
        if (!MakeReadable(*texture, error)) {
            texture->Release();
            return nullptr;
        }
        pending_[texture] = {address, mips};
        static bool reported = false;
        if (!reported) {
            reported = true;
            AbxLog("d3d12_x", "a texture at title address 0x%llx has memory that is not mapped yet (%s); it is created empty and filled when a later view finds the memory mapped",
                   static_cast<unsigned long long>(address), error.c_str());
        }
        error.clear();
    }
    textures_.emplace(std::make_pair(address, std::move(key)), texture);
    AbxTrace("title texture at 0x%llx: %llux%u, %u mips, format %d uploaded from title memory (later writes there are not seen yet)", static_cast<unsigned long long>(address),
             static_cast<unsigned long long>(applied.Width), applied.Height, mips, applied.Format);
    return texture;
}

bool TitleTextures::MakeReadable(Resource& texture, std::string& error) {
    bool recorded = true;
    const bool submitted = context_.device.Submit(
        [&](VkCommandBuffer commands) {
            CommandRecorder recorder(context_.device, commands);
            recorded = recorder.Transition(texture.Texture(), D3D12_RESOURCE_STATE_COPY_DEST, kShaderRead, error);
        },
        error);
    return submitted && recorded;
}

TitleTextures::Filled TitleTextures::Upload(Resource& texture, std::uint64_t address, std::uint32_t mips, std::string& error) {
    const D3D12_RESOURCE_DESC& desc = texture.Texture().desc;
    const UINT layers = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
    const UINT count = mips * layers;
    const std::optional<Footprints> footprints = DesktopCopyableFootprints(desc, mips, 0, count, 0, error);
    if (!footprints) return Filled::Failed;
    if (!Readable(address, footprints->totalBytes)) {
        error = "the texture's title memory is not readable for " + std::to_string(footprints->totalBytes) + " bytes";
        return Filled::NotMapped;
    }
    const bool alreadyReadable = pending_.count(&texture) != 0;
    std::optional<std::uint64_t> offset;
    BufferRef staging;
    std::optional<TitleAddressSpace::Location> imported;
    if (context_.addresses != nullptr) imported = context_.addresses->Locate(address, footprints->totalBytes);
    if (imported) {
        staging = {imported->buffer, imported->offset, footprints->totalBytes};
    } else {
        offset = context_.arena.Allocate(footprints->totalBytes, kBufferPlacementAlignment);
        if (!offset) {
            error = "the buffer window has no room to stage a texture of " + std::to_string(footprints->totalBytes) + " bytes, and the texture is not in imported title memory";
            return Filled::Failed;
        }
        std::memcpy(context_.window.Base() + *offset, reinterpret_cast<const void*>(address), static_cast<std::size_t>(footprints->totalBytes));
        staging = {context_.window.Buffer(), *offset, footprints->totalBytes};
    }
    bool recorded = true;
    const bool submitted = context_.device.Submit(
        [&](VkCommandBuffer commands) {
            CommandRecorder recorder(context_.device, commands);
            if (alreadyReadable) recorded = recorder.Transition(texture.Texture(), kShaderRead, D3D12_RESOURCE_STATE_COPY_DEST, error);
            for (UINT subresource = 0; subresource < count && recorded; ++subresource) {
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = footprints->layouts[subresource];
                recorded = recorder.CopyBufferToTexture(texture.Texture(), D3D12_RESOURCE_STATE_COPY_DEST, subresource, 0, 0, 0, staging, footprint, nullptr, error);
            }
            if (recorded) recorded = recorder.Transition(texture.Texture(), D3D12_RESOURCE_STATE_COPY_DEST, kShaderRead, error);
        },
        error);
    if (offset) context_.arena.Free(*offset);
    if (!submitted || !recorded) {
        if (error.empty()) error = "uploading the title texture failed";
        return Filled::Failed;
    }
    return Filled::Yes;
}

}
