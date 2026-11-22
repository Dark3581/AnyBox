#include "VulkanBackend.hpp"
#include "VulkanLoader.hpp"

#include <algorithm>
#include <cstdio>
#include <set>

namespace abx::d3d12x {

namespace {

struct InstanceFunctions {
    PFN_vkEnumeratePhysicalDevices enumerateDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties properties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 properties2 = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 features2 = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties extensions = nullptr;
};

InstanceFunctions LoadFunctions(PFN_vkGetInstanceProcAddr loader, VkInstance instance, bool version11) {
    InstanceFunctions vk;
    vk.enumerateDevices = LoadInstanceFunction<PFN_vkEnumeratePhysicalDevices>(loader, instance, "vkEnumeratePhysicalDevices");
    vk.properties = LoadInstanceFunction<PFN_vkGetPhysicalDeviceProperties>(loader, instance, "vkGetPhysicalDeviceProperties");
    vk.extensions = LoadInstanceFunction<PFN_vkEnumerateDeviceExtensionProperties>(loader, instance, "vkEnumerateDeviceExtensionProperties");
    if (version11) {
        vk.properties2 = LoadInstanceFunction<PFN_vkGetPhysicalDeviceProperties2>(loader, instance, "vkGetPhysicalDeviceProperties2");
        vk.features2 = LoadInstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>(loader, instance, "vkGetPhysicalDeviceFeatures2");
    }
    return vk;
}

const char* TypeName(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
    }
}

std::vector<std::string> DeviceExtensions(const InstanceFunctions& vk, VkPhysicalDevice device) {
    std::uint32_t count = 0;
    if (vk.extensions(device, nullptr, &count, nullptr) != VK_SUCCESS) {
        return {};
    }
    std::vector<VkExtensionProperties> properties(count);
    if (vk.extensions(device, nullptr, &count, properties.data()) != VK_SUCCESS) {
        return {};
    }
    std::vector<std::string> names;
    for (std::uint32_t index = 0; index < count; ++index) {
        names.emplace_back(properties[index].extensionName);
    }
    std::sort(names.begin(), names.end());
    return names;
}

GpuCapabilities Describe(const InstanceFunctions& vk, VkPhysicalDevice device) {
    GpuCapabilities gpu;
    VkPhysicalDeviceProperties basic{};
    vk.properties(device, &basic);
    gpu.name = basic.deviceName;
    gpu.type = TypeName(basic.deviceType);
    gpu.apiVersion = basic.apiVersion;
    gpu.driverVersion = basic.driverVersion;
    gpu.vendorId = basic.vendorID;
    gpu.deviceId = basic.deviceID;
    gpu.maxPushConstantsSize = basic.limits.maxPushConstantsSize;
    gpu.extensions = DeviceExtensions(vk, device);
    const std::set<std::string> available(gpu.extensions.begin(), gpu.extensions.end());
    const bool v12 = basic.apiVersion >= VK_API_VERSION_1_2;
    const bool v13 = basic.apiVersion >= VK_API_VERSION_1_3;
    if (basic.apiVersion < VK_API_VERSION_1_1 || vk.properties2 == nullptr || vk.features2 == nullptr) {
        return gpu;
    }

    VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceSubgroupSizeControlProperties sizeControl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    if (v13) {
        subgroup.pNext = &sizeControl;
    }
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &subgroup};
    vk.properties2(device, &properties);
    gpu.subgroup.size = subgroup.subgroupSize;
    gpu.subgroup.stages = subgroup.supportedStages;
    gpu.subgroup.operations = subgroup.supportedOperations;
    if (v13) {
        gpu.subgroup.minSize = sizeControl.minSubgroupSize;
        gpu.subgroup.maxSize = sizeControl.maxSubgroupSize;
        gpu.subgroup.requiredSizeStages = sizeControl.requiredSubgroupSizeStages;
    }

    VkPhysicalDeviceVulkan12Features vulkan12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features vulkan13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentric{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptorBuffer{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    void** next = &features.pNext;
    const auto chain = [&next](auto& structure) {
        *next = &structure;
        next = &structure.pNext;
    };
    if (v12) chain(vulkan12);
    if (v13) chain(vulkan13);
    const bool hasMesh = available.count(VK_EXT_MESH_SHADER_EXTENSION_NAME) != 0;
    const bool hasBarycentric = available.count(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME) != 0;
    const bool hasInterlock = available.count(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME) != 0;
    const bool hasDescriptorBuffer = available.count(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME) != 0;
    if (hasMesh) chain(mesh);
    if (hasBarycentric) chain(barycentric);
    if (hasInterlock) chain(interlock);
    if (hasDescriptorBuffer) chain(descriptorBuffer);
    vk.features2(device, &features);

    const VkPhysicalDeviceFeatures& core = features.features;
    auto& f = gpu.features;
    f["geometryShader"] = core.geometryShader;
    f["shaderClipDistance"] = core.shaderClipDistance;
    f["shaderCullDistance"] = core.shaderCullDistance;
    f["shaderFloat64"] = core.shaderFloat64;
    f["shaderInt64"] = core.shaderInt64;
    f["shaderInt16"] = core.shaderInt16;
    f["shaderImageGatherExtended"] = core.shaderImageGatherExtended;
    f["shaderResourceMinLod"] = core.shaderResourceMinLod;
    f["shaderStorageImageReadWithoutFormat"] = core.shaderStorageImageReadWithoutFormat;
    f["shaderStorageImageWriteWithoutFormat"] = core.shaderStorageImageWriteWithoutFormat;
    f["bufferDeviceAddress"] = v12 && vulkan12.bufferDeviceAddress;
    f["descriptorIndexing"] = v12 && vulkan12.descriptorIndexing;
    f["runtimeDescriptorArray"] = v12 && vulkan12.runtimeDescriptorArray;
    f["descriptorBindingPartiallyBound"] = v12 && vulkan12.descriptorBindingPartiallyBound;
    f["shaderFloat16"] = v12 && vulkan12.shaderFloat16;
    f["scalarBlockLayout"] = v12 && vulkan12.scalarBlockLayout;
    f["timelineSemaphore"] = v12 && vulkan12.timelineSemaphore;
    f["vulkanMemoryModel"] = v12 && vulkan12.vulkanMemoryModel;
    f["subgroupSizeControl"] = v13 && vulkan13.subgroupSizeControl;
    f["computeFullSubgroups"] = v13 && vulkan13.computeFullSubgroups;
    f["synchronization2"] = v13 && vulkan13.synchronization2;
    f["dynamicRendering"] = v13 && vulkan13.dynamicRendering;
    f["maintenance4"] = v13 && vulkan13.maintenance4;
    f["meshShader"] = hasMesh && mesh.meshShader;
    f["taskShader"] = hasMesh && mesh.taskShader;
    f["fragmentShaderBarycentric"] = hasBarycentric && barycentric.fragmentShaderBarycentric;
    f["fragmentShaderPixelInterlock"] = hasInterlock && interlock.fragmentShaderPixelInterlock;
    f["fragmentShaderSampleInterlock"] = hasInterlock && interlock.fragmentShaderSampleInterlock;
    f["descriptorBuffer"] = hasDescriptorBuffer && descriptorBuffer.descriptorBuffer;
    return gpu;
}

void AppendEscaped(std::string& out, const std::string& text) {
    out += '"';
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
            out += escaped;
        } else {
            out += c;
        }
    }
    out += '"';
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "\"0x%x\"", static_cast<unsigned>(value));
    return text;
}

}

VulkanReport QueryVulkan() {
    VulkanReport report;
    const VulkanLibrary library;
    const PFN_vkGetInstanceProcAddr loader = library.Loader();
    if (loader == nullptr) {
        report.error = "vulkan-1.dll is not installed";
        return report;
    }
    const auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(loader(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    report.instanceVersion = VK_API_VERSION_1_0;
    if (enumerateVersion != nullptr && enumerateVersion(&report.instanceVersion) != VK_SUCCESS) {
        report.instanceVersion = VK_API_VERSION_1_0;
    }
    const auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(loader(VK_NULL_HANDLE, "vkCreateInstance"));
    if (createInstance == nullptr) {
        report.error = "vkCreateInstance is not exported";
        return report;
    }
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "AnyBox";
    application.pEngineName = "AnyBox";
    application.apiVersion = std::min<std::uint32_t>(report.instanceVersion, VK_API_VERSION_1_3);
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &application;
    VkInstance raw = VK_NULL_HANDLE;
    const VkResult created = createInstance(&create, nullptr, &raw);
    if (created != VK_SUCCESS) {
        report.error = "vkCreateInstance failed with " + std::to_string(static_cast<int>(created));
        return report;
    }
    const VulkanInstance instance(raw, LoadInstanceFunction<PFN_vkDestroyInstance>(loader, raw, "vkDestroyInstance"));
    const InstanceFunctions vk = LoadFunctions(loader, raw, application.apiVersion >= VK_API_VERSION_1_1);
    if (vk.enumerateDevices == nullptr || vk.properties == nullptr || vk.extensions == nullptr) {
        report.error = "the Vulkan loader lacks core instance functions";
        return report;
    }
    std::uint32_t count = 0;
    if (vk.enumerateDevices(instance.Get(), &count, nullptr) != VK_SUCCESS) {
        report.error = "vkEnumeratePhysicalDevices failed";
        return report;
    }
    std::vector<VkPhysicalDevice> devices(count);
    if (count > 0 && vk.enumerateDevices(instance.Get(), &count, devices.data()) < VK_SUCCESS) {
        report.error = "vkEnumeratePhysicalDevices failed";
        return report;
    }
    devices.resize(count);
    for (const VkPhysicalDevice device : devices) {
        report.devices.push_back(Describe(vk, device));
    }
    if (report.devices.empty()) {
        report.error = "no Vulkan device";
        return report;
    }
    report.available = true;
    return report;
}

GpuCapabilities DescribePhysicalDevice(PFN_vkGetInstanceProcAddr loader, VkInstance instance, VkPhysicalDevice device) {
    return Describe(LoadFunctions(loader, instance, true), device);
}

std::string VersionText(std::uint32_t version) {
    return std::to_string(VK_API_VERSION_MAJOR(version)) + "." + std::to_string(VK_API_VERSION_MINOR(version)) + "." + std::to_string(VK_API_VERSION_PATCH(version));
}

std::string ToJson(const VulkanReport& report) {
    std::string out = "{\n  \"format\": \"anybox-gpu-report\",\n  \"version\": 1,\n  \"available\": ";
    out += report.available ? "true" : "false";
    out += ",\n  \"error\": ";
    if (report.error.empty()) {
        out += "null";
    } else {
        AppendEscaped(out, report.error);
    }
    out += ",\n  \"instanceVersion\": \"" + VersionText(report.instanceVersion) + "\",\n  \"devices\": [";
    for (std::size_t index = 0; index < report.devices.size(); ++index) {
        const GpuCapabilities& gpu = report.devices[index];
        out += index == 0 ? "\n    {" : ",\n    {";
        out += "\"name\": ";
        AppendEscaped(out, gpu.name);
        out += ", \"type\": \"" + gpu.type + "\", \"apiVersion\": \"" + VersionText(gpu.apiVersion) + "\", \"driverVersion\": " + Hex(gpu.driverVersion);
        out += ", \"vendorId\": " + Hex(gpu.vendorId) + ", \"deviceId\": " + Hex(gpu.deviceId);
        out += ",\n     \"maxPushConstantsSize\": " + std::to_string(gpu.maxPushConstantsSize);
        out += ", \"subgroup\": {\"size\": " + std::to_string(gpu.subgroup.size) + ", \"minSize\": " + std::to_string(gpu.subgroup.minSize) +
               ", \"maxSize\": " + std::to_string(gpu.subgroup.maxSize) + ", \"stages\": " + Hex(gpu.subgroup.stages) +
               ", \"operations\": " + Hex(gpu.subgroup.operations) + ", \"requiredSizeStages\": " + Hex(gpu.subgroup.requiredSizeStages) + "}";
        out += ",\n     \"features\": {";
        bool first = true;
        for (const auto& [name, supported] : gpu.features) {
            out += first ? "" : ", ";
            first = false;
            AppendEscaped(out, name);
            out += supported ? ": true" : ": false";
        }
        out += "},\n     \"extensions\": [";
        for (std::size_t extension = 0; extension < gpu.extensions.size(); ++extension) {
            out += extension == 0 ? "" : ", ";
            AppendEscaped(out, gpu.extensions[extension]);
        }
        out += "]}";
    }
    out += report.devices.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

}
