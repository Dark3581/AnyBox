# d3d12_x

## add Functional

- [ViewDesc](../../../core/libs/xdll/d3d12_x/resources/ViewDesc.cpp) (d3d12_x) - turns desktop shader resource, unordered access, render target and depth-stencil view descriptions of textures into `VkImageViewCreateInfo`, and sampler descriptions into `VkSamplerCreateInfo`. A view format must be in the texture's format family; depth textures keep their depth format and select the depth or stencil aspect from the view format; `B8G8R8X8` views read alpha as 1. Refused: buffer views (they go through the descriptor heap), minimum LOD clamps, plane slices other than the stencil plane, unordered access views of part of a 3D texture's depth, and border colors other than transparent black, opaque black and opaque white. Depth-stencil view read-only flags are accepted and left to the layout chosen when the view is bound. On the console a view is written into a descriptor heap as console descriptor words the shaders read; how those words relate to these descriptions is unknown, and nothing creates views until the device's view methods exist
  Closes when: a title's views and samplers are created through it and its shaders sample the right data
