# d3d12_x

## change [creates compute pipelines only, keeps every pipeline]

- [PipelineLibrary](../../../core/libs/xdll/d3d12_x/vulkan/PipelineLibrary.cpp) (d3d12_x) - keeps every pipeline (and every refusal or failure) until the library is destroyed, and its run-time compile limit has no default: the caller picks it. For graphics pipelines the limit counts the bytes of all their modules together, and the driver's cache hit feedback is not recorded
  Closes when: the device sets the compile limit from a configuration value and drops pipelines the title releases

## change [creates graphics pipelines with dynamic rendering]

- [GraphicsPipeline](../../../core/libs/xdll/d3d12_x/vulkan/GraphicsPipeline.cpp) (d3d12_x) - creates graphics pipelines with dynamic rendering for vertex, geometry and pixel shaders; mesh and amplification shaders (pipeline state streams) are not handled
  Closes when: a title's mesh pipelines are created from its pipeline state streams

## add Functional

- [GraphicsPipelineRequest](../../../core/libs/xdll/d3d12_x/pso/PipelineState.cpp) (d3d12_x) - resolves a graphics pipeline state description's vertex, geometry and pixel shaders through the store and combines them with its render state and input layout into one cache-first library request, keyed by the shader keys and the translated state; a cached pipeline blob is ignored and the root signature is not consulted. Nothing calls it until the device's `CreateGraphicsPipelineState` exists
  Closes when: the device's `CreateGraphicsPipelineState` creates its pipelines through it
