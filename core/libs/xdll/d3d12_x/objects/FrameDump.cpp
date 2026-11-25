#include "FrameDump.hpp"

#include "AbxApi.hpp"
#include "BindlessSet.hpp"
#include "DescriptorHeap.hpp"
#include "Resource.hpp"
#include "VulkanDevice.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace abx::d3d12x {

namespace {

std::mutex g_lock;
std::set<Resource*> g_targets;
std::set<std::uint32_t> g_tables;

bool FourBytes(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return true;
    default: return false;
    }
}

void Dump(const VulkanDevice& device, Resource* resource, const std::string& path) {
    const TextureRef& texture = resource->Texture();
    const D3D12_RESOURCE_DESC& desc = texture.desc;
    if (texture.image == VK_NULL_HANDLE || !FourBytes(desc.Format) || desc.Width == 0 || desc.Height == 0) {
        AbxLog("d3d12_x", "frame dump: %s skipped (format %d is not a 4-byte color format)", path.c_str(), static_cast<int>(desc.Format));
        return;
    }
    const std::uint32_t width = static_cast<std::uint32_t>(desc.Width);
    const std::uint32_t height = desc.Height;
    std::string error;
    DeviceBuffer staging;
    if (!CreateHostBuffer(device, static_cast<VkDeviceSize>(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, staging, error)) {
        AbxLog("d3d12_x", "frame dump: %s failed: %s", path.c_str(), error.c_str());
        return;
    }
    const bool copied = device.Submit(
        [&](VkCommandBuffer commands) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {width, height, 1};
            device.Vk().cmdCopyImageToBuffer(commands, texture.image, VK_IMAGE_LAYOUT_GENERAL, staging.buffer, 1, &region);
        },
        error);
    if (!copied) {
        AbxLog("d3d12_x", "frame dump: %s failed: %s", path.c_str(), error.c_str());
        DestroyBuffer(device, staging);
        return;
    }
    constexpr std::uint32_t kStep = 4;
    const std::uint32_t outWidth = width / kStep;
    const std::uint32_t outHeight = height / kStep;
    const std::uint32_t rowBytes = (outWidth * 3 + 3) & ~3u;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(rowBytes) * outHeight);
    const auto* source = static_cast<const std::uint8_t*>(staging.mapped);
    const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
                      desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
    std::uint64_t lit = 0;
    for (std::uint32_t y = 0; y < outHeight; ++y) {
        for (std::uint32_t x = 0; x < outWidth; ++x) {
            const std::uint8_t* texel = source + (static_cast<std::size_t>(y) * kStep * width + static_cast<std::size_t>(x) * kStep) * 4;
            std::uint8_t* out = pixels.data() + static_cast<std::size_t>(outHeight - 1 - y) * rowBytes + static_cast<std::size_t>(x) * 3;
            out[0] = bgra ? texel[0] : texel[2];
            out[1] = texel[1];
            out[2] = bgra ? texel[2] : texel[0];
            if (static_cast<unsigned>(texel[0]) + texel[1] + texel[2] > 30) ++lit;
        }
    }
    DestroyBuffer(device, staging);
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return;
    const std::uint32_t imageBytes = static_cast<std::uint32_t>(pixels.size());
    const std::uint32_t fileBytes = 54 + imageBytes;
    std::uint8_t header[54] = {'B', 'M'};
    std::memcpy(header + 2, &fileBytes, 4);
    const std::uint32_t offset = 54, infoBytes = 40, planesBits = 1u | (24u << 16);
    std::memcpy(header + 10, &offset, 4);
    std::memcpy(header + 14, &infoBytes, 4);
    std::memcpy(header + 18, &outWidth, 4);
    std::memcpy(header + 22, &outHeight, 4);
    std::memcpy(header + 26, &planesBits, 4);
    std::memcpy(header + 34, &imageBytes, 4);
    std::fwrite(header, 1, sizeof(header), file);
    std::fwrite(pixels.data(), 1, pixels.size(), file);
    std::fclose(file);
    AbxLog("d3d12_x", "frame dump: %s (%ux%u, format %d, %llu of %u sampled pixels not dark)", path.c_str(), width, height, static_cast<int>(desc.Format),
           static_cast<unsigned long long>(lit), outWidth * outHeight);
}

std::string Folder() {
    wchar_t module[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    std::string folder;
    for (DWORD index = 0; index < length; ++index) folder.push_back(static_cast<char>(module[index]));
    const std::size_t slash = folder.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : folder.substr(0, slash);
}

}

void NoteTracedTable(std::uint32_t gpu) {
    const std::lock_guard guard(g_lock);
    g_tables.insert(gpu);
}

void NoteTracedTarget(Resource* target) {
    if (target == nullptr) return;
    const std::lock_guard guard(g_lock);
    g_targets.insert(target);
}

void DumpTracedFrame(const VulkanDevice& device, Resource* presented, std::uint64_t frame) {
    std::set<Resource*> targets;
    std::set<std::uint32_t> tables;
    {
        const std::lock_guard guard(g_lock);
        targets.swap(g_targets);
        tables.swap(g_tables);
    }
    for (const std::uint32_t gpu : tables) {
        UINT increment = 0;
        const std::uint8_t* table = DescriptorHeap::AtGpuAddress(gpu, increment);
        if (table == nullptr) continue;
        std::uint32_t slots[16] = {};
        std::memcpy(slots, table, sizeof(slots));
        AbxLog("d3d12_x", "TRACE     table 0x%x at present: %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x", gpu, slots[0], slots[1], slots[2], slots[3],
               slots[4], slots[5], slots[6], slots[7], slots[8], slots[9], slots[10], slots[11], slots[12], slots[13], slots[14], slots[15]);
    }
    const std::string folder = Folder();
    char name[160];
    std::snprintf(name, sizeof(name), "%s\\frame%llu_present.bmp", folder.c_str(), static_cast<unsigned long long>(frame));
    Dump(device, presented, name);
    int index = 0;
    for (Resource* target : targets) {
        std::snprintf(name, sizeof(name), "%s\\frame%llu_target%d_%p.bmp", folder.c_str(), static_cast<unsigned long long>(frame), index++,
                      static_cast<void*>(target->Texture().image));
        Dump(device, target, name);
    }
}

}
