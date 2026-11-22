# d3d12_x

## add Functional

- [PipelineState](../../../core/libs/xdll/d3d12_x/pso/PipelineState.cpp) (d3d12_x) - resolves a compute pipeline state description to its stored module and refuses any pipeline state flag and any node other than the first; the root signature is not consulted because every pipeline uses the title layout, and a cached pipeline blob is ignored. Nothing calls it until the device's `CreateComputePipelineState` exists
  Closes when: the device's `CreateComputePipelineState` creates its pipelines through it
- [PipelineLibrary](../../../core/libs/xdll/d3d12_x/vulkan/PipelineLibrary.cpp) (d3d12_x) - creates compute pipelines only, keeps every pipeline (and every refusal or failure) until the library is destroyed, and its run-time compile limit has no default: the caller picks it
  Closes when: graphics pipelines go through it and the device sets the compile limit from a configuration value
