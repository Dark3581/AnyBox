# d3d12_x

## add Functional

- [Resource](../../../core/libs/xdll/d3d12_x/objects/Resource.cpp) (d3d12_x) - committed `ID3D12Resource`s: buffers in the title buffer window at 64 KiB alignment, mappable whatever their heap type (default-heap buffers therefore live in host memory, which is slow but correct), zeroed unless `CREATE_NOT_ZEROED`; textures as images with their own device memory, cleared to zero (except block-compressed ones) and moved into their initial state at creation with a synchronous submission. Placed and reserved resources, custom heaps, shared and protected heaps, textures in upload and readback heaps, mapping textures and `WriteToSubresource`/`ReadFromSubresource` are refused or stop the title. Whether the console's committed buffers need other alignments or addresses is unknown. Nothing creates resources until the device's `CreateCommittedResource` exists
  Closes when: a title's resources are created through it, default-heap buffers move to device-local memory, and placed resources are supported
- [BufferArena](../../../core/libs/xdll/d3d12_x/vulkan/BufferArena.cpp) (d3d12_x) - first-fit allocation over the buffer window with freed ranges merged back; no defragmentation, and the window's size is the caller's choice
  Closes when: the device sizes the window from a title's needs and its allocation pattern is seen not to fragment it
