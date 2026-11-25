#include "VulkanDevice.hpp"
#include "TitleLayout.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace abx::d3d12x {

namespace {

struct FeatureChain {
    VkPhysicalDeviceFeatures2 core{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentric{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    VkPhysicalDeviceMaintenance8FeaturesKHR maintenance8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR};
    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamic3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceFaultFeaturesEXT fault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};

    FeatureChain() = default;
    FeatureChain(const FeatureChain&) = delete;
    FeatureChain& operator=(const FeatureChain&) = delete;

    void Link(const std::set<std::string>& extensions) {
        void** next = &core.pNext;
        const auto chain = [&next](auto& structure) {
            *next = &structure;
            next = &structure.pNext;
        };
        chain(v12);
        chain(v13);
        if (extensions.count(VK_EXT_MESH_SHADER_EXTENSION_NAME) != 0) chain(mesh);
        if (extensions.count(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME) != 0) chain(barycentric);
        if (extensions.count(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME) != 0) chain(interlock);
        if (extensions.count(VK_KHR_MAINTENANCE_8_EXTENSION_NAME) != 0) chain(maintenance8);
        if (extensions.count(VK_KHR_MAINTENANCE_5_EXTENSION_NAME) != 0) chain(maintenance5);
        if (extensions.count(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME) != 0) chain(dynamic3);
        if (extensions.count(VK_EXT_DEVICE_FAULT_EXTENSION_NAME) != 0) chain(fault);
        *next = nullptr;
    }
};

using FeatureField = VkBool32& (*)(FeatureChain&);

struct FeatureEntry {
    const char* name;
    const char* extension;
    FeatureField field;
};

const FeatureEntry kFeatures[] = {
    {"geometryShader", nullptr, [](FeatureChain& c) -> VkBool32& { return c.core.features.geometryShader; }},
    {"shaderClipDistance", nullptr, [](FeatureChain& c) -> VkBool32& { return c.core.features.shaderClipDistance; }},
    {"shaderCullDistance", nullptr, [](FeatureChain& c) -> VkBool32& { return c.core.features.shaderCullDistance; }},
    {"shaderFloat64", nullptr, [](FeatureChain& c) -> VkBool32& { return c.core.features.shaderFloat64; }},
    {"shaderInt64", nullptr, [](FeatureChain& c) -> VkBool32& { return c.core.features.shaderInt64; }},
    {"shaderInt16", nullptr, [](FeatureChain& c) -> VkBool32& { return c.core.features.shaderInt16; }},
    {"shaderImageGatherExtended", nullptr, [](FeatureChain& c) -> VkBool32& { return c.core.features.shaderImageGatherExtended; }},
    {"shaderResourceMinLod", nullptr, [](FeatureChain& c) -> VkBool32& { return c.core.features.shaderResourceMinLod; }},
    {"shaderStorageImageReadWithoutFormat", nullptr, [](FeatureChain& c) -> VkBool32& { return c.core.features.shaderStorageImageReadWithoutFormat; }},
    {"shaderStorageImageWriteWithoutFormat", nullptr, [](FeatureChain& c) -> VkBool32& { return c.core.features.shaderStorageImageWriteWithoutFormat; }},
    {"bufferDeviceAddress", nullptr, [](FeatureChain& c) -> VkBool32& { return c.v12.bufferDeviceAddress; }},
    {"runtimeDescriptorArray", nullptr, [](FeatureChain& c) -> VkBool32& { return c.v12.runtimeDescriptorArray; }},
    {"shaderFloat16", nullptr, [](FeatureChain& c) -> VkBool32& { return c.v12.shaderFloat16; }},
    {"scalarBlockLayout", nullptr, [](FeatureChain& c) -> VkBool32& { return c.v12.scalarBlockLayout; }},
    {"vulkanMemoryModel", nullptr, [](FeatureChain& c) -> VkBool32& { return c.v12.vulkanMemoryModel; }},
    {"meshShader", VK_EXT_MESH_SHADER_EXTENSION_NAME, [](FeatureChain& c) -> VkBool32& { return c.mesh.meshShader; }},
    {"fragmentShaderBarycentric", VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME, [](FeatureChain& c) -> VkBool32& { return c.barycentric.fragmentShaderBarycentric; }},
    {"fragmentShaderPixelInterlock", VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME, [](FeatureChain& c) -> VkBool32& { return c.interlock.fragmentShaderPixelInterlock; }},
};

const FeatureEntry kLayoutFeatures[] = {
    {"descriptorBindingPartiallyBound", nullptr, [](FeatureChain& c) -> VkBool32& { return c.v12.descriptorBindingPartiallyBound; }},
    {"descriptorBindingSampledImageUpdateAfterBind", nullptr, [](FeatureChain& c) -> VkBool32& { return c.v12.descriptorBindingSampledImageUpdateAfterBind; }},
    {"descriptorBindingStorageImageUpdateAfterBind", nullptr, [](FeatureChain& c) -> VkBool32& { return c.v12.descriptorBindingStorageImageUpdateAfterBind; }},
    {"dynamicRendering", nullptr, [](FeatureChain& c) -> VkBool32& { return c.v13.dynamicRendering; }},
    {"synchronization2", nullptr, [](FeatureChain& c) -> VkBool32& { return c.v13.synchronization2; }},
    {"timelineSemaphore", nullptr, [](FeatureChain& c) -> VkBool32& { return c.v12.timelineSemaphore; }},
};

const FeatureEntry kOptionalFeatures[] = {
    {"subgroupSizeControl", nullptr, [](FeatureChain& c) -> VkBool32& { return c.v13.subgroupSizeControl; }},
    {"computeFullSubgroups", nullptr, [](FeatureChain& c) -> VkBool32& { return c.v13.computeFullSubgroups; }},
    {"pipelineCreationCacheControl", nullptr, [](FeatureChain& c) -> VkBool32& { return c.v13.pipelineCreationCacheControl; }},
    {"fillModeNonSolid", nullptr, [](FeatureChain& c) -> VkBool32& { return c.core.features.fillModeNonSolid; }},
    {"logicOp", nullptr, [](FeatureChain& c) -> VkBool32& { return c.core.features.logicOp; }},
    {"depthClamp", nullptr, [](FeatureChain& c) -> VkBool32& { return c.core.features.depthClamp; }},
    {"depthBiasClamp", nullptr, [](FeatureChain& c) -> VkBool32& { return c.core.features.depthBiasClamp; }},
    {"independentBlend", nullptr, [](FeatureChain& c) -> VkBool32& { return c.core.features.independentBlend; }},
    {"dualSrcBlend", nullptr, [](FeatureChain& c) -> VkBool32& { return c.core.features.dualSrcBlend; }},
    {"textureCompressionBC", nullptr, [](FeatureChain& c) -> VkBool32& { return c.core.features.textureCompressionBC; }},
    {"imageCubeArray", nullptr, [](FeatureChain& c) -> VkBool32& { return c.core.features.imageCubeArray; }},
    {"samplerAnisotropy", nullptr, [](FeatureChain& c) -> VkBool32& { return c.core.features.samplerAnisotropy; }},
    {"samplerFilterMinmax", nullptr, [](FeatureChain& c) -> VkBool32& { return c.v12.samplerFilterMinmax; }},
    {"samplerMirrorClampToEdge", nullptr, [](FeatureChain& c) -> VkBool32& { return c.v12.samplerMirrorClampToEdge; }},
    {"multiDrawIndirect", nullptr, [](FeatureChain& c) -> VkBool32& { return c.core.features.multiDrawIndirect; }},
    {"drawIndirectCount", nullptr, [](FeatureChain& c) -> VkBool32& { return c.v12.drawIndirectCount; }},
};

const FeatureEntry* FindFeature(const std::string& name) {
    for (const FeatureEntry& entry : kFeatures) {
        if (name == entry.name) {
            return &entry;
        }
    }
    return nullptr;
}

struct LayoutDemand {
    std::uint64_t sampledImages = 0;
    std::uint64_t samplers = 0;
    std::uint64_t storageImages = 0;
    std::uint64_t storageBuffers = 0;
};

LayoutDemand Demand() {
    LayoutDemand demand;
    for (const BindingSpec& spec : TitleBindings()) {
        switch (spec.kind) {
        case DescriptorKind::SampledImage: demand.sampledImages += spec.count; break;
        case DescriptorKind::Sampler: demand.samplers += spec.count; break;
        case DescriptorKind::StorageImage: demand.storageImages += spec.count; break;
        case DescriptorKind::StorageBuffer: demand.storageBuffers += spec.count; break;
        }
    }
    return demand;
}

std::string LimitProblem(const VkPhysicalDeviceProperties& properties, const VkPhysicalDeviceVulkan12Properties& limits) {
    const LayoutDemand demand = Demand();
    const std::uint64_t total = demand.sampledImages + demand.samplers + demand.storageImages + demand.storageBuffers;
    const struct {
        const char* what;
        std::uint64_t need;
        std::uint64_t have;
    } checks[] = {
        {"push constant bytes", kTitlePushConstantBytes, properties.limits.maxPushConstantsSize},
        {"update-after-bind sampled images per stage", demand.sampledImages, limits.maxPerStageDescriptorUpdateAfterBindSampledImages},
        {"update-after-bind sampled images per set", demand.sampledImages, limits.maxDescriptorSetUpdateAfterBindSampledImages},
        {"update-after-bind samplers per stage", demand.samplers, limits.maxPerStageDescriptorUpdateAfterBindSamplers},
        {"update-after-bind samplers per set", demand.samplers, limits.maxDescriptorSetUpdateAfterBindSamplers},
        {"update-after-bind storage images per stage", demand.storageImages, limits.maxPerStageDescriptorUpdateAfterBindStorageImages},
        {"update-after-bind storage images per set", demand.storageImages, limits.maxDescriptorSetUpdateAfterBindStorageImages},
        {"update-after-bind storage buffers per stage", demand.storageBuffers, limits.maxPerStageDescriptorUpdateAfterBindStorageBuffers},
        {"update-after-bind resources per stage", total, limits.maxPerStageUpdateAfterBindResources},
    };
    for (const auto& check : checks) {
        if (check.need > check.have) {
            return std::string(check.what) + ": the title layout needs " + std::to_string(check.need) + ", the device allows " + std::to_string(check.have);
        }
    }
    return {};
}

std::set<std::string> ExtensionNames(PFN_vkEnumerateDeviceExtensionProperties enumerate, VkPhysicalDevice device) {
    std::uint32_t count = 0;
    if (enumerate(device, nullptr, &count, nullptr) != VK_SUCCESS) {
        return {};
    }
    std::vector<VkExtensionProperties> properties(count);
    if (enumerate(device, nullptr, &count, properties.data()) != VK_SUCCESS) {
        return {};
    }
    std::set<std::string> names;
    for (std::uint32_t index = 0; index < count; ++index) {
        names.insert(properties[index].extensionName);
    }
    return names;
}

VkDescriptorType DescriptorType(DescriptorKind kind) {
    switch (kind) {
    case DescriptorKind::SampledImage: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case DescriptorKind::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
    case DescriptorKind::StorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case DescriptorKind::StorageImage: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}

template <typename Function>
Function DeviceFunction(PFN_vkGetDeviceProcAddr loader, VkDevice device, const char* name) {
    return reinterpret_cast<Function>(loader(device, name));
}

}

const std::vector<std::string>& KnownShaderFeatures() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> list;
        for (const FeatureEntry& entry : kFeatures) {
            list.emplace_back(entry.name);
        }
        return list;
    }();
    return names;
}

std::unique_ptr<VulkanDevice> VulkanDevice::Create(const DeviceRequirements& requirements, std::string& error) {
    std::unique_ptr<VulkanDevice> device(new VulkanDevice());
    if (!device->Open(requirements, error) || !device->CreateLayout(error)) {
        return nullptr;
    }
    return device;
}

VulkanDevice::~VulkanDevice() {
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vk_.destroyPipelineLayout(device_, pipelineLayout_, nullptr);
    }
    if (setLayout_ != VK_NULL_HANDLE) {
        vk_.destroyDescriptorSetLayout(device_, setLayout_, nullptr);
    }
    if (device_ != VK_NULL_HANDLE) {
        vk_.destroyDevice(device_, nullptr);
    }
    if (messenger_ != VK_NULL_HANDLE && destroyMessenger_ != nullptr) {
        destroyMessenger_(instance_.Get(), messenger_, nullptr);
    }
    instance_.Reset();
}

namespace {

constexpr int kPreferredBonus = 100;

std::string Lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

}

int DeviceScore(VkPhysicalDeviceType type, VkDriverId driver, const std::string& name, const std::string& preferred) {
    int score = 0;
    if (driver == VK_DRIVER_ID_MESA_DOZEN) {
        score = 2;
    } else if (type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        score = 4;
    } else if (type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        score = 3;
    } else if (type == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) {
        score = 1;
    }
    if (!preferred.empty() && Lower(name).find(Lower(preferred)) != std::string::npos) {
        score += kPreferredBonus;
    }
    return score;
}

bool VulkanDevice::Open(const DeviceRequirements& requirements, std::string& error) {
    for (const std::string& name : requirements.features) {
        if (FindFeature(name) == nullptr) {
            error = "unknown shader feature " + name;
            return false;
        }
    }
    const PFN_vkGetInstanceProcAddr loader = library_.Loader();
    if (loader == nullptr) {
        error = "vulkan-1.dll is not installed";
        return false;
    }
    const auto enumerateVersion = LoadInstanceFunction<PFN_vkEnumerateInstanceVersion>(loader, VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    std::uint32_t instanceVersion = VK_API_VERSION_1_0;
    if (enumerateVersion == nullptr || enumerateVersion(&instanceVersion) != VK_SUCCESS || instanceVersion < VK_API_VERSION_1_3) {
        error = "the Vulkan loader does not offer Vulkan 1.3";
        return false;
    }
    const auto createInstance = LoadInstanceFunction<PFN_vkCreateInstance>(loader, VK_NULL_HANDLE, "vkCreateInstance");
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "AnyBox";
    application.pEngineName = "AnyBox";
    application.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &application;
    const char* validationLayer = "VK_LAYER_KHRONOS_validation";
    const char* debugExtension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkDebugUtilsMessengerCreateInfoEXT messengerInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    messengerInfo.pfnUserCallback = &VulkanDevice::OnMessage;
    messengerInfo.pUserData = this;
    std::vector<const char*> instanceExtensions;
    if (requirements.validation) {
        const auto enumerateLayers = LoadInstanceFunction<PFN_vkEnumerateInstanceLayerProperties>(loader, VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
        std::uint32_t layerCount = 0;
        bool found = false;
        if (enumerateLayers != nullptr && enumerateLayers(&layerCount, nullptr) == VK_SUCCESS) {
            std::vector<VkLayerProperties> layers(layerCount);
            if (enumerateLayers(&layerCount, layers.data()) >= VK_SUCCESS) {
                for (std::uint32_t index = 0; index < layerCount; ++index) {
                    found = found || std::strcmp(layers[index].layerName, validationLayer) == 0;
                }
            }
        }
        if (!found) {
            error = "the Khronos validation layer is not installed";
            return false;
        }
        instanceInfo.enabledLayerCount = 1;
        instanceInfo.ppEnabledLayerNames = &validationLayer;
        instanceExtensions.push_back(debugExtension);
        instanceInfo.pNext = &messengerInfo;
    }
    if (requirements.presentation) {
        instanceExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        instanceExtensions.push_back("VK_KHR_win32_surface");
    }
    instanceInfo.enabledExtensionCount = static_cast<std::uint32_t>(instanceExtensions.size());
    instanceInfo.ppEnabledExtensionNames = instanceExtensions.empty() ? nullptr : instanceExtensions.data();
    VkInstance rawInstance = VK_NULL_HANDLE;
    const VkResult instanceResult = createInstance == nullptr ? VK_ERROR_INITIALIZATION_FAILED : createInstance(&instanceInfo, nullptr, &rawInstance);
    if (instanceResult != VK_SUCCESS) {
        error = "vkCreateInstance failed with " + std::to_string(static_cast<int>(instanceResult));
        return false;
    }
    instance_.Assign(rawInstance, LoadInstanceFunction<PFN_vkDestroyInstance>(loader, rawInstance, "vkDestroyInstance"));
    if (requirements.validation) {
        const auto createMessenger = LoadInstanceFunction<PFN_vkCreateDebugUtilsMessengerEXT>(loader, rawInstance, "vkCreateDebugUtilsMessengerEXT");
        destroyMessenger_ = LoadInstanceFunction<PFN_vkDestroyDebugUtilsMessengerEXT>(loader, rawInstance, "vkDestroyDebugUtilsMessengerEXT");
        if (createMessenger == nullptr || destroyMessenger_ == nullptr || createMessenger(rawInstance, &messengerInfo, nullptr, &messenger_) != VK_SUCCESS) {
            messenger_ = VK_NULL_HANDLE;
            error = "vkCreateDebugUtilsMessengerEXT failed";
            return false;
        }
    }
    const auto enumerateDevices = LoadInstanceFunction<PFN_vkEnumeratePhysicalDevices>(loader, rawInstance, "vkEnumeratePhysicalDevices");
    const auto getProperties2 = LoadInstanceFunction<PFN_vkGetPhysicalDeviceProperties2>(loader, rawInstance, "vkGetPhysicalDeviceProperties2");
    const auto getFeatures2 = LoadInstanceFunction<PFN_vkGetPhysicalDeviceFeatures2>(loader, rawInstance, "vkGetPhysicalDeviceFeatures2");
    const auto getQueueFamilies = LoadInstanceFunction<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(loader, rawInstance, "vkGetPhysicalDeviceQueueFamilyProperties");
    const auto enumerateExtensions = LoadInstanceFunction<PFN_vkEnumerateDeviceExtensionProperties>(loader, rawInstance, "vkEnumerateDeviceExtensionProperties");
    const auto createDevice = LoadInstanceFunction<PFN_vkCreateDevice>(loader, rawInstance, "vkCreateDevice");
    const auto getDeviceProcAddr = LoadInstanceFunction<PFN_vkGetDeviceProcAddr>(loader, rawInstance, "vkGetDeviceProcAddr");
    if (enumerateDevices == nullptr || getProperties2 == nullptr || getFeatures2 == nullptr || getQueueFamilies == nullptr || enumerateExtensions == nullptr ||
        createDevice == nullptr || getDeviceProcAddr == nullptr) {
        error = "the Vulkan loader lacks core instance functions";
        return false;
    }
    std::uint32_t count = 0;
    if (enumerateDevices(rawInstance, &count, nullptr) != VK_SUCCESS || count == 0) {
        error = "no Vulkan device";
        return false;
    }
    std::vector<VkPhysicalDevice> candidates(count);
    if (enumerateDevices(rawInstance, &count, candidates.data()) < VK_SUCCESS) {
        error = "vkEnumeratePhysicalDevices failed";
        return false;
    }
    candidates.resize(count);

    int bestScore = -1;
    std::set<std::string> bestExtensions;
    std::string reasons;
    for (const VkPhysicalDevice candidate : candidates) {
        VkPhysicalDeviceVulkan12Properties limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &limits};
        getProperties2(candidate, &properties);
        const std::string name = properties.properties.deviceName;
        const auto reject = [&reasons, &name](const std::string& why) { reasons += (reasons.empty() ? "" : "; ") + name + ": " + why; };
        if (properties.properties.apiVersion < VK_API_VERSION_1_3) {
            reject("no Vulkan 1.3");
            continue;
        }
        const std::set<std::string> extensions = ExtensionNames(enumerateExtensions, candidate);
        FeatureChain supported;
        supported.Link(extensions);
        getFeatures2(candidate, &supported.core);
        std::string missing;
        for (const std::string& feature : requirements.features) {
            const FeatureEntry* entry = FindFeature(feature);
            if ((entry->extension != nullptr && extensions.count(entry->extension) == 0) || !entry->field(supported)) {
                missing += (missing.empty() ? "" : ", ") + feature;
            }
        }
        for (const FeatureEntry& entry : kLayoutFeatures) {
            if (!entry.field(supported)) {
                missing += (missing.empty() ? "" : ", ") + std::string(entry.name);
            }
        }
        if (requirements.hostMemoryImport && extensions.count(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) == 0) {
            missing += (missing.empty() ? "" : ", ") + std::string(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
        }
        if (requirements.maintenance8 && (extensions.count(VK_KHR_MAINTENANCE_8_EXTENSION_NAME) == 0 || !supported.maintenance8.maintenance8)) {
            missing += (missing.empty() ? "" : ", ") + std::string("maintenance8");
        }
        if (!missing.empty()) {
            reject("lacks " + missing);
            continue;
        }
        const std::string limitProblem = LimitProblem(properties.properties, limits);
        if (!limitProblem.empty()) {
            reject(limitProblem);
            continue;
        }
        std::uint32_t familyCount = 0;
        getQueueFamilies(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        getQueueFamilies(candidate, &familyCount, families.data());
        std::uint32_t family = familyCount;
        for (std::uint32_t index = 0; index < familyCount; ++index) {
            const VkQueueFlags flags = families[index].queueFlags;
            if ((flags & VK_QUEUE_GRAPHICS_BIT) != 0 && (flags & VK_QUEUE_COMPUTE_BIT) != 0 && families[index].queueCount > 0) {
                family = index;
                break;
            }
        }
        if (family == familyCount) {
            reject("no queue family with graphics and compute");
            continue;
        }
        const int score = DeviceScore(properties.properties.deviceType, limits.driverID, name, requirements.preferredDevice);
        if (score > bestScore) {
            bestScore = score;
            physical_ = candidate;
            properties_ = properties.properties;
            limits12_ = limits;
            limits12_.pNext = nullptr;
            queueFamily_ = family;
            bestExtensions = extensions;
        }
    }
    if (physical_ == VK_NULL_HANDLE) {
        error = "no Vulkan device can run the title's shaders: " + reasons;
        return false;
    }
    preferenceMissed_ = !requirements.preferredDevice.empty() && bestScore < kPreferredBonus;

    gpu_ = DescribePhysicalDevice(loader, rawInstance, physical_);
    FeatureChain supported;
    supported.Link(bestExtensions);
    getFeatures2(physical_, &supported.core);
    FeatureChain enabled;
    std::set<std::string> useExtensions;
    bufferDeviceAddress_ = requirements.features.count("bufferDeviceAddress") != 0;
    for (const std::string& feature : requirements.features) {
        const FeatureEntry* entry = FindFeature(feature);
        entry->field(enabled) = VK_TRUE;
        enabled_.push_back(feature);
        if (entry->extension != nullptr) {
            useExtensions.insert(entry->extension);
        }
    }
    for (const FeatureEntry& entry : kLayoutFeatures) {
        entry.field(enabled) = VK_TRUE;
        enabled_.emplace_back(entry.name);
    }
    for (const FeatureEntry& entry : kOptionalFeatures) {
        if (entry.field(supported)) {
            entry.field(enabled) = VK_TRUE;
            enabled_.emplace_back(entry.name);
        }
    }
    subgroupSizeControl_ = supported.v13.subgroupSizeControl == VK_TRUE;
    pipelineCacheControl_ = supported.v13.pipelineCreationCacheControl == VK_TRUE;
    if (requirements.hostMemoryImport) {
        useExtensions.insert(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT host{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 hostProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &host};
        getProperties2(physical_, &hostProperties);
        hostImportAlignment_ = host.minImportedHostPointerAlignment;
    }
    if (bestExtensions.count(VK_EXT_MESH_SHADER_EXTENSION_NAME) != 0) {
        VkPhysicalDeviceMeshShaderPropertiesEXT mesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 meshProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &mesh};
        getProperties2(physical_, &meshProperties);
        for (int axis = 0; axis < 3; ++axis) meshMaxGroups_[axis] = mesh.maxMeshWorkGroupCount[axis];
        meshMaxTotal_ = mesh.maxMeshWorkGroupTotalCount;
    }
    if (requirements.presentation) {
        if (bestExtensions.count(VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
            error = "the device does not offer " VK_KHR_SWAPCHAIN_EXTENSION_NAME;
            return false;
        }
        useExtensions.insert(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    if (bestExtensions.count(VK_KHR_MAINTENANCE_5_EXTENSION_NAME) != 0) {
        enabled.maintenance5.maintenance5 = VK_TRUE;
        enabled_.emplace_back("maintenance5");
        useExtensions.insert(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    }
    if (bestExtensions.count(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME) != 0 && bestExtensions.count(VK_NV_FILL_RECTANGLE_EXTENSION_NAME) != 0 &&
        supported.dynamic3.extendedDynamicState3PolygonMode == VK_TRUE) {
        enabled.dynamic3.extendedDynamicState3PolygonMode = VK_TRUE;
        enabled_.emplace_back("rectangleFill");
        useExtensions.insert(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
        useExtensions.insert(VK_NV_FILL_RECTANGLE_EXTENSION_NAME);
        rectangleFill_ = true;
    }
    if (bestExtensions.count(VK_EXT_DEVICE_FAULT_EXTENSION_NAME) != 0 && supported.fault.deviceFault == VK_TRUE) {
        enabled.fault.deviceFault = VK_TRUE;
        enabled.fault.deviceFaultVendorBinary = VK_FALSE;
        useExtensions.insert(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        deviceFault_ = true;
    }
    if (bestExtensions.count(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME) != 0) {
        useExtensions.insert(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);
        enabled_.emplace_back("conservativeRasterization");
        conservative_ = true;
    }
    if (bestExtensions.count(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME) != 0) {
        const char* value = std::getenv("ANYBOX_CHECKPOINTS");
        if (value != nullptr && value[0] == '1') {
            useExtensions.insert(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
            checkpoints_ = true;
        }
    }
    if (requirements.maintenance8) {
        enabled.maintenance8.maintenance8 = VK_TRUE;
        enabled_.emplace_back("maintenance8");
        useExtensions.insert(VK_KHR_MAINTENANCE_8_EXTENSION_NAME);
    }
    enabled.Link(useExtensions);
    std::sort(enabled_.begin(), enabled_.end());
    extensions_.assign(useExtensions.begin(), useExtensions.end());
    std::vector<const char*> extensionNames;
    for (const std::string& extension : extensions_) {
        extensionNames.push_back(extension.c_str());
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = queueFamily_;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enabled.core};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queue;
    deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensionNames.size());
    deviceInfo.ppEnabledExtensionNames = extensionNames.empty() ? nullptr : extensionNames.data();
    const VkResult deviceResult = createDevice(physical_, &deviceInfo, nullptr, &device_);
    if (deviceResult != VK_SUCCESS) {
        device_ = VK_NULL_HANDLE;
        error = "vkCreateDevice failed with " + std::to_string(static_cast<int>(deviceResult));
        return false;
    }
    instanceLoader_ = loader;
    deviceLoader_ = getDeviceProcAddr;
    if (deviceFault_) faultInfo_ = DeviceFunction<PFN_vkGetDeviceFaultInfoEXT>(getDeviceProcAddr, device_, "vkGetDeviceFaultInfoEXT");
    if (checkpoints_) {
        setCheckpoint_ = DeviceFunction<PFN_vkCmdSetCheckpointNV>(getDeviceProcAddr, device_, "vkCmdSetCheckpointNV");
        checkpointData_ = DeviceFunction<PFN_vkGetQueueCheckpointDataNV>(getDeviceProcAddr, device_, "vkGetQueueCheckpointDataNV");
    }
    vk_.destroyDevice = DeviceFunction<PFN_vkDestroyDevice>(getDeviceProcAddr, device_, "vkDestroyDevice");
    vk_.getDeviceQueue = DeviceFunction<PFN_vkGetDeviceQueue>(getDeviceProcAddr, device_, "vkGetDeviceQueue");
    vk_.createShaderModule = DeviceFunction<PFN_vkCreateShaderModule>(getDeviceProcAddr, device_, "vkCreateShaderModule");
    vk_.destroyShaderModule = DeviceFunction<PFN_vkDestroyShaderModule>(getDeviceProcAddr, device_, "vkDestroyShaderModule");
    vk_.createComputePipelines = DeviceFunction<PFN_vkCreateComputePipelines>(getDeviceProcAddr, device_, "vkCreateComputePipelines");
    vk_.createGraphicsPipelines = DeviceFunction<PFN_vkCreateGraphicsPipelines>(getDeviceProcAddr, device_, "vkCreateGraphicsPipelines");
    vk_.destroyPipeline = DeviceFunction<PFN_vkDestroyPipeline>(getDeviceProcAddr, device_, "vkDestroyPipeline");
    vk_.createPipelineCache = DeviceFunction<PFN_vkCreatePipelineCache>(getDeviceProcAddr, device_, "vkCreatePipelineCache");
    vk_.destroyPipelineCache = DeviceFunction<PFN_vkDestroyPipelineCache>(getDeviceProcAddr, device_, "vkDestroyPipelineCache");
    vk_.getPipelineCacheData = DeviceFunction<PFN_vkGetPipelineCacheData>(getDeviceProcAddr, device_, "vkGetPipelineCacheData");
    vk_.createDescriptorSetLayout = DeviceFunction<PFN_vkCreateDescriptorSetLayout>(getDeviceProcAddr, device_, "vkCreateDescriptorSetLayout");
    vk_.destroyDescriptorSetLayout = DeviceFunction<PFN_vkDestroyDescriptorSetLayout>(getDeviceProcAddr, device_, "vkDestroyDescriptorSetLayout");
    vk_.createPipelineLayout = DeviceFunction<PFN_vkCreatePipelineLayout>(getDeviceProcAddr, device_, "vkCreatePipelineLayout");
    vk_.destroyPipelineLayout = DeviceFunction<PFN_vkDestroyPipelineLayout>(getDeviceProcAddr, device_, "vkDestroyPipelineLayout");
    vk_.createBuffer = DeviceFunction<PFN_vkCreateBuffer>(getDeviceProcAddr, device_, "vkCreateBuffer");
    vk_.destroyBuffer = DeviceFunction<PFN_vkDestroyBuffer>(getDeviceProcAddr, device_, "vkDestroyBuffer");
    vk_.getBufferMemoryRequirements = DeviceFunction<PFN_vkGetBufferMemoryRequirements>(getDeviceProcAddr, device_, "vkGetBufferMemoryRequirements");
    vk_.allocateMemory = DeviceFunction<PFN_vkAllocateMemory>(getDeviceProcAddr, device_, "vkAllocateMemory");
    vk_.freeMemory = DeviceFunction<PFN_vkFreeMemory>(getDeviceProcAddr, device_, "vkFreeMemory");
    vk_.bindBufferMemory = DeviceFunction<PFN_vkBindBufferMemory>(getDeviceProcAddr, device_, "vkBindBufferMemory");
    vk_.mapMemory = DeviceFunction<PFN_vkMapMemory>(getDeviceProcAddr, device_, "vkMapMemory");
    vk_.unmapMemory = DeviceFunction<PFN_vkUnmapMemory>(getDeviceProcAddr, device_, "vkUnmapMemory");
    vk_.createImage = DeviceFunction<PFN_vkCreateImage>(getDeviceProcAddr, device_, "vkCreateImage");
    vk_.destroyImage = DeviceFunction<PFN_vkDestroyImage>(getDeviceProcAddr, device_, "vkDestroyImage");
    vk_.getImageMemoryRequirements = DeviceFunction<PFN_vkGetImageMemoryRequirements>(getDeviceProcAddr, device_, "vkGetImageMemoryRequirements");
    vk_.bindImageMemory = DeviceFunction<PFN_vkBindImageMemory>(getDeviceProcAddr, device_, "vkBindImageMemory");
    vk_.createImageView = DeviceFunction<PFN_vkCreateImageView>(getDeviceProcAddr, device_, "vkCreateImageView");
    vk_.destroyImageView = DeviceFunction<PFN_vkDestroyImageView>(getDeviceProcAddr, device_, "vkDestroyImageView");
    vk_.createSampler = DeviceFunction<PFN_vkCreateSampler>(getDeviceProcAddr, device_, "vkCreateSampler");
    vk_.destroySampler = DeviceFunction<PFN_vkDestroySampler>(getDeviceProcAddr, device_, "vkDestroySampler");
    vk_.createDescriptorPool = DeviceFunction<PFN_vkCreateDescriptorPool>(getDeviceProcAddr, device_, "vkCreateDescriptorPool");
    vk_.destroyDescriptorPool = DeviceFunction<PFN_vkDestroyDescriptorPool>(getDeviceProcAddr, device_, "vkDestroyDescriptorPool");
    vk_.allocateDescriptorSets = DeviceFunction<PFN_vkAllocateDescriptorSets>(getDeviceProcAddr, device_, "vkAllocateDescriptorSets");
    vk_.updateDescriptorSets = DeviceFunction<PFN_vkUpdateDescriptorSets>(getDeviceProcAddr, device_, "vkUpdateDescriptorSets");
    vk_.createCommandPool = DeviceFunction<PFN_vkCreateCommandPool>(getDeviceProcAddr, device_, "vkCreateCommandPool");
    vk_.destroyCommandPool = DeviceFunction<PFN_vkDestroyCommandPool>(getDeviceProcAddr, device_, "vkDestroyCommandPool");
    vk_.resetCommandPool = DeviceFunction<PFN_vkResetCommandPool>(getDeviceProcAddr, device_, "vkResetCommandPool");
    vk_.allocateCommandBuffers = DeviceFunction<PFN_vkAllocateCommandBuffers>(getDeviceProcAddr, device_, "vkAllocateCommandBuffers");
    vk_.beginCommandBuffer = DeviceFunction<PFN_vkBeginCommandBuffer>(getDeviceProcAddr, device_, "vkBeginCommandBuffer");
    vk_.endCommandBuffer = DeviceFunction<PFN_vkEndCommandBuffer>(getDeviceProcAddr, device_, "vkEndCommandBuffer");
    vk_.cmdPipelineBarrier = DeviceFunction<PFN_vkCmdPipelineBarrier>(getDeviceProcAddr, device_, "vkCmdPipelineBarrier");
    vk_.cmdPipelineBarrier2 = DeviceFunction<PFN_vkCmdPipelineBarrier2>(getDeviceProcAddr, device_, "vkCmdPipelineBarrier2");
    vk_.cmdBindPipeline = DeviceFunction<PFN_vkCmdBindPipeline>(getDeviceProcAddr, device_, "vkCmdBindPipeline");
    vk_.cmdBindDescriptorSets = DeviceFunction<PFN_vkCmdBindDescriptorSets>(getDeviceProcAddr, device_, "vkCmdBindDescriptorSets");
    vk_.cmdPushConstants = DeviceFunction<PFN_vkCmdPushConstants>(getDeviceProcAddr, device_, "vkCmdPushConstants");
    vk_.cmdDispatch = DeviceFunction<PFN_vkCmdDispatch>(getDeviceProcAddr, device_, "vkCmdDispatch");
    vk_.cmdClearColorImage = DeviceFunction<PFN_vkCmdClearColorImage>(getDeviceProcAddr, device_, "vkCmdClearColorImage");
    vk_.cmdClearDepthStencilImage = DeviceFunction<PFN_vkCmdClearDepthStencilImage>(getDeviceProcAddr, device_, "vkCmdClearDepthStencilImage");
    vk_.cmdBeginRendering = DeviceFunction<PFN_vkCmdBeginRendering>(getDeviceProcAddr, device_, "vkCmdBeginRendering");
    vk_.cmdEndRendering = DeviceFunction<PFN_vkCmdEndRendering>(getDeviceProcAddr, device_, "vkCmdEndRendering");
    vk_.cmdClearAttachments = DeviceFunction<PFN_vkCmdClearAttachments>(getDeviceProcAddr, device_, "vkCmdClearAttachments");
    vk_.cmdCopyBuffer = DeviceFunction<PFN_vkCmdCopyBuffer>(getDeviceProcAddr, device_, "vkCmdCopyBuffer");
    vk_.cmdFillBuffer = DeviceFunction<PFN_vkCmdFillBuffer>(getDeviceProcAddr, device_, "vkCmdFillBuffer");
    vk_.cmdUpdateBuffer = DeviceFunction<PFN_vkCmdUpdateBuffer>(getDeviceProcAddr, device_, "vkCmdUpdateBuffer");
    vk_.cmdCopyBufferToImage = DeviceFunction<PFN_vkCmdCopyBufferToImage>(getDeviceProcAddr, device_, "vkCmdCopyBufferToImage");
    vk_.cmdCopyImageToBuffer = DeviceFunction<PFN_vkCmdCopyImageToBuffer>(getDeviceProcAddr, device_, "vkCmdCopyImageToBuffer");
    vk_.cmdCopyImage = DeviceFunction<PFN_vkCmdCopyImage>(getDeviceProcAddr, device_, "vkCmdCopyImage");
    vk_.cmdResolveImage = DeviceFunction<PFN_vkCmdResolveImage>(getDeviceProcAddr, device_, "vkCmdResolveImage");
    vk_.cmdSetViewportWithCount = DeviceFunction<PFN_vkCmdSetViewportWithCount>(getDeviceProcAddr, device_, "vkCmdSetViewportWithCount");
    vk_.cmdSetScissorWithCount = DeviceFunction<PFN_vkCmdSetScissorWithCount>(getDeviceProcAddr, device_, "vkCmdSetScissorWithCount");
    vk_.cmdSetPrimitiveTopology = DeviceFunction<PFN_vkCmdSetPrimitiveTopology>(getDeviceProcAddr, device_, "vkCmdSetPrimitiveTopology");
    vk_.cmdSetPrimitiveRestartEnable = DeviceFunction<PFN_vkCmdSetPrimitiveRestartEnable>(getDeviceProcAddr, device_, "vkCmdSetPrimitiveRestartEnable");
    if (rectangleFill_) vk_.cmdSetPolygonMode = DeviceFunction<PFN_vkCmdSetPolygonModeEXT>(getDeviceProcAddr, device_, "vkCmdSetPolygonModeEXT");
    vk_.cmdSetBlendConstants = DeviceFunction<PFN_vkCmdSetBlendConstants>(getDeviceProcAddr, device_, "vkCmdSetBlendConstants");
    vk_.cmdSetStencilReference = DeviceFunction<PFN_vkCmdSetStencilReference>(getDeviceProcAddr, device_, "vkCmdSetStencilReference");
    vk_.cmdBindVertexBuffers2 = DeviceFunction<PFN_vkCmdBindVertexBuffers2>(getDeviceProcAddr, device_, "vkCmdBindVertexBuffers2");
    vk_.cmdBindIndexBuffer = DeviceFunction<PFN_vkCmdBindIndexBuffer>(getDeviceProcAddr, device_, "vkCmdBindIndexBuffer");
    vk_.cmdDraw = DeviceFunction<PFN_vkCmdDraw>(getDeviceProcAddr, device_, "vkCmdDraw");
    vk_.cmdDrawIndexed = DeviceFunction<PFN_vkCmdDrawIndexed>(getDeviceProcAddr, device_, "vkCmdDrawIndexed");
    vk_.cmdDrawIndirect = DeviceFunction<PFN_vkCmdDrawIndirect>(getDeviceProcAddr, device_, "vkCmdDrawIndirect");
    vk_.cmdDrawIndexedIndirect = DeviceFunction<PFN_vkCmdDrawIndexedIndirect>(getDeviceProcAddr, device_, "vkCmdDrawIndexedIndirect");
    vk_.cmdDrawIndirectCount = DeviceFunction<PFN_vkCmdDrawIndirectCount>(getDeviceProcAddr, device_, "vkCmdDrawIndirectCount");
    vk_.cmdDrawIndexedIndirectCount = DeviceFunction<PFN_vkCmdDrawIndexedIndirectCount>(getDeviceProcAddr, device_, "vkCmdDrawIndexedIndirectCount");
    vk_.cmdDispatchIndirect = DeviceFunction<PFN_vkCmdDispatchIndirect>(getDeviceProcAddr, device_, "vkCmdDispatchIndirect");
    vk_.cmdDrawMeshTasks = DeviceFunction<PFN_vkCmdDrawMeshTasksEXT>(getDeviceProcAddr, device_, "vkCmdDrawMeshTasksEXT");
    vk_.cmdDrawMeshTasksIndirect = DeviceFunction<PFN_vkCmdDrawMeshTasksIndirectEXT>(getDeviceProcAddr, device_, "vkCmdDrawMeshTasksIndirectEXT");
    vk_.cmdDrawMeshTasksIndirectCount = DeviceFunction<PFN_vkCmdDrawMeshTasksIndirectCountEXT>(getDeviceProcAddr, device_, "vkCmdDrawMeshTasksIndirectCountEXT");
    vk_.queueSubmit = DeviceFunction<PFN_vkQueueSubmit>(getDeviceProcAddr, device_, "vkQueueSubmit");
    vk_.createSemaphore = DeviceFunction<PFN_vkCreateSemaphore>(getDeviceProcAddr, device_, "vkCreateSemaphore");
    vk_.destroySemaphore = DeviceFunction<PFN_vkDestroySemaphore>(getDeviceProcAddr, device_, "vkDestroySemaphore");
    vk_.getSemaphoreCounterValue = DeviceFunction<PFN_vkGetSemaphoreCounterValue>(getDeviceProcAddr, device_, "vkGetSemaphoreCounterValue");
    vk_.signalSemaphore = DeviceFunction<PFN_vkSignalSemaphore>(getDeviceProcAddr, device_, "vkSignalSemaphore");
    vk_.waitSemaphores = DeviceFunction<PFN_vkWaitSemaphores>(getDeviceProcAddr, device_, "vkWaitSemaphores");
    vk_.queueSubmit2 = DeviceFunction<PFN_vkQueueSubmit2>(getDeviceProcAddr, device_, "vkQueueSubmit2");
    vk_.createFence = DeviceFunction<PFN_vkCreateFence>(getDeviceProcAddr, device_, "vkCreateFence");
    vk_.destroyFence = DeviceFunction<PFN_vkDestroyFence>(getDeviceProcAddr, device_, "vkDestroyFence");
    vk_.waitForFences = DeviceFunction<PFN_vkWaitForFences>(getDeviceProcAddr, device_, "vkWaitForFences");
    vk_.getFenceStatus = DeviceFunction<PFN_vkGetFenceStatus>(getDeviceProcAddr, device_, "vkGetFenceStatus");
    vk_.getBufferDeviceAddress = DeviceFunction<PFN_vkGetBufferDeviceAddress>(getDeviceProcAddr, device_, "vkGetBufferDeviceAddress");
    vk_.queueWaitIdle = DeviceFunction<PFN_vkQueueWaitIdle>(getDeviceProcAddr, device_, "vkQueueWaitIdle");
    if (vk_.destroyDevice == nullptr) {
        error = "vkDestroyDevice is not available";
        return false;
    }
    const void* const* functions = reinterpret_cast<const void* const*>(&vk_);
    for (std::size_t index = 0; index < sizeof(vk_) / sizeof(void*); ++index) {
        if (functions[index] == nullptr) {
            error = "a device function is not available";
            return false;
        }
    }
    vk_.getDeviceQueue(device_, queueFamily_, 0, &queue_);
    const auto getMemoryProperties = LoadInstanceFunction<PFN_vkGetPhysicalDeviceMemoryProperties>(loader, rawInstance, "vkGetPhysicalDeviceMemoryProperties");
    if (getMemoryProperties == nullptr || queue_ == VK_NULL_HANDLE) {
        error = "no device queue or memory properties";
        return false;
    }
    getMemoryProperties(physical_, &memory_);
    formatProperties_ = LoadInstanceFunction<PFN_vkGetPhysicalDeviceFormatProperties>(loader, rawInstance, "vkGetPhysicalDeviceFormatProperties");
    if (formatProperties_ == nullptr) {
        error = "vkGetPhysicalDeviceFormatProperties is not available";
        return false;
    }
    if (requirements.hostMemoryImport) {
        hostPointerProperties_ = DeviceFunction<PFN_vkGetMemoryHostPointerPropertiesEXT>(getDeviceProcAddr, device_, "vkGetMemoryHostPointerPropertiesEXT");
        if (hostPointerProperties_ == nullptr || hostImportAlignment_ == 0) {
            error = "vkGetMemoryHostPointerPropertiesEXT is not available";
            return false;
        }
    }
    return true;
}

bool VulkanDevice::CreateLayout(std::string& error) {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    std::vector<VkDescriptorBindingFlags> flags;
    for (const BindingSpec& spec : TitleBindings()) {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = spec.binding;
        binding.descriptorType = DescriptorType(spec.kind);
        binding.descriptorCount = spec.count;
        binding.stageFlags = VK_SHADER_STAGE_ALL;
        bindings.push_back(binding);
        flags.push_back(spec.kind == DescriptorKind::StorageBuffer ? 0
                                                                   : VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bindingFlags.bindingCount = static_cast<std::uint32_t>(flags.size());
    bindingFlags.pBindingFlags = flags.data();
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bindingFlags};
    setInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    setInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    setInfo.pBindings = bindings.data();
    VkResult result = vk_.createDescriptorSetLayout(device_, &setInfo, nullptr, &setLayout_);
    if (result != VK_SUCCESS) {
        setLayout_ = VK_NULL_HANDLE;
        error = "vkCreateDescriptorSetLayout failed with " + std::to_string(static_cast<int>(result));
        return false;
    }
    const VkPushConstantRange range{VK_SHADER_STAGE_ALL, 0, kTitlePushConstantBytes};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    result = vk_.createPipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_);
    if (result != VK_SUCCESS) {
        pipelineLayout_ = VK_NULL_HANDLE;
        error = "vkCreatePipelineLayout failed with " + std::to_string(static_cast<int>(result));
        return false;
    }
    return true;
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDevice::OnMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                                      const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    VulkanDevice* device = static_cast<VulkanDevice*>(user);
    if (device != nullptr && (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        std::lock_guard<std::mutex> guard(device->messagesLock_);
        ++device->validationErrors_;
        if (device->validationMessages_.size() < 20) {
            device->validationMessages_.emplace_back(data != nullptr && data->pMessage != nullptr ? data->pMessage : "(no message)");
        }
    }
    return VK_FALSE;
}

std::size_t VulkanDevice::ValidationErrors() const {
    std::lock_guard<std::mutex> guard(messagesLock_);
    return validationErrors_;
}

std::vector<std::string> VulkanDevice::ValidationMessages() const {
    std::lock_guard<std::mutex> guard(messagesLock_);
    return validationMessages_;
}

VkFormatFeatureFlags VulkanDevice::OptimalFeatures(VkFormat format) const {
    VkFormatProperties properties{};
    formatProperties_(physical_, format, &properties);
    return properties.optimalTilingFeatures;
}

std::optional<std::uint32_t> VulkanDevice::SystemMemoryType(std::uint32_t typeBits) const {
    for (std::uint32_t index = 0; index < memory_.memoryTypeCount; ++index) {
        const VkMemoryType& type = memory_.memoryTypes[index];
        if ((typeBits & (1u << index)) == 0 || (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) continue;
        if ((memory_.memoryHeaps[type.heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) continue;
        return index;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> VulkanDevice::MemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags required) const {
    for (std::uint32_t index = 0; index < memory_.memoryTypeCount; ++index) {
        if ((typeBits & (1u << index)) != 0 && (memory_.memoryTypes[index].propertyFlags & required) == required) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<VulkanDevice::PendingSubmit> VulkanDevice::SubmitWithoutWaiting(const std::function<void(VkCommandBuffer)>& record, std::string& error) const {
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = queueFamily_;
    PendingSubmit pending;
    if (vk_.createCommandPool(device_, &poolInfo, nullptr, &pending.pool) != VK_SUCCESS) {
        error = "vkCreateCommandPool failed";
        return std::nullopt;
    }
    const auto cleanup = [&] {
        if (pending.fence != VK_NULL_HANDLE) vk_.destroyFence(device_, pending.fence, nullptr);
        vk_.destroyCommandPool(device_, pending.pool, nullptr);
    };
    VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = pending.pool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vk_.allocateCommandBuffers(device_, &allocateInfo, &commands) != VK_SUCCESS || vk_.beginCommandBuffer(commands, &begin) != VK_SUCCESS) {
        cleanup();
        error = "cannot start a command buffer";
        return std::nullopt;
    }
    record(commands);
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    if (vk_.endCommandBuffer(commands) != VK_SUCCESS || vk_.createFence(device_, &fenceInfo, nullptr, &pending.fence) != VK_SUCCESS) {
        cleanup();
        error = "cannot finish a command buffer";
        return std::nullopt;
    }
    VkResult submitted = VK_SUCCESS;
    {
        std::lock_guard<std::mutex> guard(queueLock_);
        submitted = vk_.queueSubmit(queue_, 1, &submit, pending.fence);
    }
    if (submitted != VK_SUCCESS) {
        cleanup();
        error = "vkQueueSubmit failed with " + std::to_string(static_cast<int>(submitted));
        if (submitted == VK_ERROR_DEVICE_LOST) error += "; " + FaultReport();
        return std::nullopt;
    }
    return pending;
}

VkResult VulkanDevice::Finish(PendingSubmit& pending, bool wait) const {
    if (pending.fence == VK_NULL_HANDLE) return VK_SUCCESS;
    const VkResult state = wait ? vk_.waitForFences(device_, 1, &pending.fence, VK_TRUE, UINT64_MAX) : vk_.getFenceStatus(device_, pending.fence);
    if (state == VK_NOT_READY || state == VK_TIMEOUT) return VK_NOT_READY;
    vk_.destroyFence(device_, pending.fence, nullptr);
    vk_.destroyCommandPool(device_, pending.pool, nullptr);
    pending = {};
    return state;
}

bool VulkanDevice::Submit(const std::function<void(VkCommandBuffer)>& record, std::string& error) const {
    std::optional<PendingSubmit> pending = SubmitWithoutWaiting(record, error);
    if (!pending) return false;
    const VkResult waited = Finish(*pending, true);
    if (waited != VK_SUCCESS) {
        error = "vkWaitForFences failed with " + std::to_string(static_cast<int>(waited));
        if (waited == VK_ERROR_DEVICE_LOST) error += "; " + FaultReport();
        return false;
    }
    return true;
}

void VulkanDevice::Checkpoint(VkCommandBuffer commands, const char* marker) const {
    if (setCheckpoint_ != nullptr) setCheckpoint_(commands, marker);
}

std::string VulkanDevice::CheckpointReport() const {
    if (checkpointData_ == nullptr) return "";
    std::uint32_t count = 0;
    checkpointData_(queue_, &count, nullptr);
    std::vector<VkCheckpointDataNV> data(count, VkCheckpointDataNV{VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
    checkpointData_(queue_, &count, data.data());
    std::string report = "; checkpoints:";
    for (const VkCheckpointDataNV& entry : data) {
        char text[64];
        std::snprintf(text, sizeof(text), " [stage 0x%x] ", static_cast<unsigned>(entry.stage));
        report += text;
        report += entry.pCheckpointMarker != nullptr ? static_cast<const char*>(entry.pCheckpointMarker) : "(none)";
    }
    return report;
}

std::string VulkanDevice::FaultReport() const {
    if (faultInfo_ == nullptr) return "the device offers no fault report (VK_EXT_device_fault)" + CheckpointReport();
    VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (faultInfo_(device_, &counts, nullptr) != VK_SUCCESS) return "the device fault report could not be read";
    std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
    counts.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
    info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
    info.pVendorInfos = vendors.empty() ? nullptr : vendors.data();
    const VkResult read = faultInfo_(device_, &counts, &info);
    if (read != VK_SUCCESS && read != VK_INCOMPLETE) return "the device fault report could not be read";
    std::string report = std::string("device fault: ") + info.description;
    static const char* const kTypes[] = {"none", "read invalid", "write invalid", "execute invalid", "instruction pointer unknown", "instruction pointer invalid",
                                         "instruction pointer fault"};
    for (const VkDeviceFaultAddressInfoEXT& address : addresses) {
        char text[160];
        std::snprintf(text, sizeof(text), "; %s at 0x%llx (precision 0x%llx)",
                      static_cast<std::size_t>(address.addressType) < std::size(kTypes) ? kTypes[address.addressType] : "unknown",
                      static_cast<unsigned long long>(address.reportedAddress), static_cast<unsigned long long>(address.addressPrecision));
        report += text;
        if (faultDescriber_) report += " [" + faultDescriber_(address.reportedAddress) + "]";
    }
    for (const VkDeviceFaultVendorInfoEXT& vendor : vendors) {
        char text[320];
        std::snprintf(text, sizeof(text), "; vendor: %s (code 0x%llx, data 0x%llx)", vendor.description, static_cast<unsigned long long>(vendor.vendorFaultCode),
                      static_cast<unsigned long long>(vendor.vendorFaultData));
        report += text;
    }
    return report + CheckpointReport();
}

bool VulkanDevice::HasFeature(const std::string& name) const {
    return std::find(enabled_.begin(), enabled_.end(), name) != enabled_.end();
}

bool VulkanDevice::CanRequireSubgroupSize(std::uint32_t size, VkShaderStageFlagBits stage) const {
    return subgroupSizeControl_ && size >= gpu_.subgroup.minSize && size <= gpu_.subgroup.maxSize && (gpu_.subgroup.requiredSizeStages & stage) != 0;
}

}
