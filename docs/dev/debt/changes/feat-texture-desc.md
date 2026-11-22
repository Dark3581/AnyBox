# d3d12_x

## add Functional

- [TextureDesc](../../../core/libs/xdll/d3d12_x/resources/TextureDesc.cpp) (d3d12_x) - turns a desktop `D3D12_RESOURCE_DESC` for a texture into a `VkImageCreateInfo`: typeless formats get a format of their class plus mutable format and extended usage (views pick the real format), typeless depth families become depth formats when the texture allows depth-stencil, `B8G8R8X8` is stored as `B8G8R8A8` (its views will have to read alpha as 1), BC formats need `textureCompressionBC`, and square 2D arrays of six layers are cube compatible. Only the driver-chosen layout is accepted; row-major and 64 KiB swizzle layouts, sample qualities, and flags beyond render target, depth-stencil, unordered access, deny shader resource, cross adapter and simultaneous access (the last two are not acted on) are refused or ignored as listed. Whether the console's own texture creation path passes this description is unknown, and nothing creates images from it until the device does
  Closes when: a title's textures are created through it and its views read them
