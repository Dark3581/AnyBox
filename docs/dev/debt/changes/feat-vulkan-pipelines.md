# d3d12_x

## change [VulkanBackend]

- [VulkanBackend](../../../core/libs/xdll/d3d12_x/vulkan/VulkanBackend.cpp) (d3d12_x) - loads `vulkan-1.dll` from the system folder, creates an instance and reports each device's features, extensions, subgroup sizes and push constant limit; the feature list covers what translated shader stores have needed so far, not every Vulkan feature
  Closes when: a feature a title's store needs is reported and enabled by `VulkanDevice`

## add Functional

- [VulkanDevice](../../../core/libs/xdll/d3d12_x/vulkan/VulkanDevice.cpp) (d3d12_x) - picks a Vulkan 1.3 device that supports every shader feature a store names, enables exactly those plus the bindless layout's descriptor-indexing features and subgroup size control, and opens one graphics and compute queue; nothing calls it until `D3D12XboxCreateDevice` exists, and a feature name it does not know stops device creation
  Closes when: `D3D12XboxCreateDevice` creates its device through it
- [TitleLayout](../../../core/libs/xdll/d3d12_x/vulkan/TitleLayout.cpp) (d3d12_x) - set 0's twenty binding types and the 144-byte push constant range follow what the translated modules declare (checked against every module of one title's store by `shaders_layout`); the array sizes (8,192 2D sampled images, 2,048 samplers and so on) are AnyBox's choice, not the title's descriptor heap sizes, and nothing fills the push constants (the root-signature-to-user-SGPR mapping, heap base and code base come from the title's own calls)
  Closes when: the descriptor heap sizes and the root-signature mapping are taken from a title's calls
- [PipelineCompiler](../../../core/libs/xdll/d3d12_x/vulkan/PipelineCompiler.cpp) (d3d12_x) - creates compute pipelines only; graphics pipelines need the title's pipeline state (vertex input, render targets, blend, rasterizer), which only its pipeline creation calls give. A module whose subgroup size the device cannot require compiles without it, as the translator expects
  Closes when: graphics pipelines are created from a title's pipeline state descriptions
