# d3d12_x

## add Functional

- [CommandAllocator](../../../core/libs/xdll/d3d12_x/objects/CommandAllocator.cpp) (d3d12_x) - `ID3D12CommandAllocator` is a Vulkan command pool that hands out one primary command buffer per list `Reset` and reuses them after its own `Reset`. Resetting an allocator whose lists the GPU is still running is the title's error, as in D3D12, and is not detected. Nothing creates allocators until the device's `CreateCommandAllocator` exists
  Closes when: the device creates its allocators through it and a title's frames record on it
- [CommandList](../../../core/libs/xdll/d3d12_x/objects/CommandList.cpp) (d3d12_x) - `ID3D12GraphicsCommandList` records resource barriers (whole resources and single subresources, UAV and aliasing), buffer, texture and footprint copies, `CopyResource`, resolves, vertex and index buffers, viewports, scissors, topology, blend factor and stencil reference. `Close` adds a barrier that makes GPU writes visible to the CPU, so a readback after a fence wait sees them. Draws, dispatches, pipeline state, root signatures and arguments, descriptor heaps, render targets, clears and `ExecuteIndirect` stop the title until the descriptor heap and pipeline state objects exist; split barriers, queries, predication, stream output, tiles, bundles and an initial pipeline state stop the title. Nothing creates lists until the device's `CreateCommandList` exists
  Closes when: draws and dispatches record through it, and a title's frame renders through it

## add Silent stubs

- [CommandList](../../../core/libs/xdll/d3d12_x/objects/CommandList.cpp) (d3d12_x) - `DiscardResource`, `SetMarker`, `BeginEvent`, `EndEvent` and `ClearState` without a pipeline state do nothing: discards are a hint, markers are debugger annotations, and the list keeps no bound state for `ClearState` to reset yet
  Closes when: markers become Vulkan debug labels and `ClearState` resets the bound state the list keeps once it binds pipelines
