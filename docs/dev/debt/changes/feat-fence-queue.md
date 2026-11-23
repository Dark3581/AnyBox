# d3d12_x

## add Functional

- [Fence](../../../core/libs/xdll/d3d12_x/objects/Fence.cpp) (d3d12_x) - `ID3D12Fence1` on a Vulkan timeline semaphore. Shared and cross-adapter fences are refused, a CPU `Signal` below the completed value stops the title (a timeline cannot go back), and each `SetEventOnCompletion` with an event waits on its own thread, which is simple but costly for titles that wait every frame. Nothing creates fences until the device's `CreateFence` exists
  Closes when: the device creates its fences through it and a title's frame pacing runs on it
- [CommandQueue](../../../core/libs/xdll/d3d12_x/objects/CommandQueue.cpp) (d3d12_x) - `ID3D12CommandQueue` signals and waits on the device's single Vulkan queue, so direct, compute and copy queues run serialized. A wait for a fence value nothing has promised yet holds that queue's later work on the CPU until a GPU or CPU signal promises it, as D3D12 does, so a wait recorded before its signal does not deadlock the shared Vulkan queue. `ExecuteCommandLists` with lists, tile mappings and `GetClockCalibration` stop the title; queue priority is ignored
  Closes when: command lists execute through it, and async compute and copy queues get their own Vulkan queues where the GPU has them

## add Silent stubs

- [CommandQueue](../../../core/libs/xdll/d3d12_x/objects/CommandQueue.cpp) (d3d12_x) - `SetMarker`, `BeginEvent` and `EndEvent` do nothing: they are debugger annotations with no effect on rendering
  Closes when: markers are forwarded to Vulkan debug labels for a GPU debugger
