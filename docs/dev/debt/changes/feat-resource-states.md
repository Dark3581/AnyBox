# d3d12_x

## add Functional

- [ResourceState](../../../core/libs/xdll/d3d12_x/commands/ResourceState.cpp) (d3d12_x) - maps D3D12 resource states to Vulkan stages, access and image layouts for barriers: `COMMON` is `GENERAL` with all commands and memory access, a combination of read states takes the one layout they share or `GENERAL`, and shader resource states of a depth image use the depth read-only layout. Buffers get memory barriers, since they share one buffer. Stream output, ray tracing, shading rate and video states, a write state combined with another state and unknown bits are refused. Implicit state promotion and decay, split barriers (`BEGIN_ONLY`/`END_ONLY`), subresource-level tracking and the predication access of `INDIRECT_ARGUMENT` are not handled, and nothing records these barriers until command lists exist
  Closes when: a title's command lists record their barriers through it and its frames render without hazards
