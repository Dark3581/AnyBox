# d3d12_x

## add Functional

- [VulkanBackend](../../../core/libs/xdll/d3d12_x/vulkan/VulkanBackend.cpp) (d3d12_x) - loads `vulkan-1.dll` from the system folder, creates an instance and reports each device's features, extensions, subgroup sizes and push constant limit; it creates no device or queue yet, and the feature list covers what translated shader stores have needed so far, not every Vulkan feature
  Closes when: `D3D12XboxCreateDevice` creates a Vulkan device from these capabilities
