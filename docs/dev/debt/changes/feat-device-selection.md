# d3d12_x

## change [picks a Vulkan 1.3 device that supports every shader feature a store names]

- [VulkanDevice](../../../core/libs/xdll/d3d12_x/vulkan/VulkanDevice.cpp) (d3d12_x) - picks a Vulkan 1.3 device that supports every shader feature a store names, preferring a native discrete GPU, then a native integrated one, then a layered driver (Mesa Dozen, recognised by its driver ID), then a CPU device, unless the `d3d12_x.vulkan_device` setting names part of a device's name; a preference that matches nothing falls back to that order and `PreferenceMissed` says so. It enables exactly those features plus the bindless layout's descriptor-indexing features and subgroup size control, and opens one graphics and compute queue; nothing calls it until `D3D12XboxCreateDevice` exists, and a feature name it does not know stops device creation. Only one GPU has been tested; the ordering among several native GPUs and with Dozen installed is not seen on hardware
  Closes when: `D3D12XboxCreateDevice` creates its device through it with `DeviceRequirementsFromSettings`, and a machine with several GPUs or Dozen installed picks the expected one
