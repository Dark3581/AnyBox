# d3d12_x

## add Functional

- [RootSignatureLayout](../../../core/libs/xdll/d3d12_x/pso/RootSignatureLayout.cpp) (d3d12_x) - deserializes a root signature blob through the host `d3d12.dll` (as version 1.1) and lays out its parameters by desktop D3D12's cost (root constants one 32-bit word each, root descriptors two, descriptor tables one, 64 at most), with tables, ranges and static samplers (translated to Vulkan samplers) checked. Where each of those words lands in a translated shader's user registers, and how a console table pointer or root descriptor is sized there, is not known; this layout is not that mapping. Register overlaps are left to the serializer, and blobs with the console's flag bits 24 to 31 are rejected by the host as before. Nothing builds it until the device's `CreateRootSignature` exists
  Closes when: the root-signature-to-user-register mapping is known from a title's shaders and calls, and the device builds its root signatures through it
