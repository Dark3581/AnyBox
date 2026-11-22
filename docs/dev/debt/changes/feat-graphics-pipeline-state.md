# d3d12_x

## add Functional

- [GraphicsState](../../../core/libs/xdll/d3d12_x/pso/GraphicsState.cpp) (d3d12_x) - translates the render state of a graphics pipeline state description only: shaders, the input layout and the root signature are not handled, and tessellation, stream output, conservative rasterization, forced sample counts, sample qualities other than 0 and the standard pattern, pipeline state flags and formats without a Vulkan mapping (console-only formats included) are refused with an error naming them. The integer depth bias is passed as Vulkan's constant factor, which matches D3D12 for fixed-point depth formats but not exactly for floating-point ones; `MultisampleEnable` and `AntialiasedLineEnable` (line rasterization modes) are ignored; `D24_UNORM_S8_UINT` is passed through although some GPUs cannot render to it; the front face assumes viewports set with a negative height so D3D12 winding applies
  Closes when: graphics pipelines are created from a title's descriptions with its shaders and input layout, and each refusal has met a title or been implemented
- [GraphicsPipeline](../../../core/libs/xdll/d3d12_x/vulkan/GraphicsPipeline.cpp) (d3d12_x) - creates graphics pipelines with dynamic rendering, no pipeline cache lookup of its own and no deduplication; the pipeline library covers compute only
  Closes when: graphics pipelines go through the pipeline library
