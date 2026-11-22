# d3d12_x

## add Functional

- [VertexInput](../../../core/libs/xdll/d3d12_x/pso/VertexInput.cpp) (d3d12_x) - an input layout element feeds the vertex shader input with the same semantic name (compared without case) and index, at the location the translator gives that input register; elements the shader does not read are left out. `D3D12_APPEND_ALIGNED_ELEMENT` places an element after the previous one in its slot, aligned to the smaller of 4 bytes and its own size, which D3D12 documents only as "directly after the previous one, including any packing". Instance step rates other than 1, an element whose format class (float, unsigned or signed integer) differs from the shader input's type, a shader input with no element and formats without a Vulkan vertex mapping are refused with an error naming them. Strides are left to draw time
  Closes when: a title's input layouts create its graphics pipelines and its draws read the right vertex data
