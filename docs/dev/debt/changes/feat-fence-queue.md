# d3d12_x

## add Functional

- [Fence](../../../core/libs/xdll/d3d12_x/objects/Fence.cpp) (d3d12_x) - `ID3D12Fence1` on a Vulkan timeline semaphore. Shared and cross-adapter fences are refused, a CPU `Signal` below the completed value stops the title (a timeline cannot go back), and events from `SetEventOnCompletion` are served by one waiter thread per fence (started on the first event wait, woken through a private timeline semaphore, stopped and joined when the fence is released, which drops events still pending); on a lost device every pending event is signalled. Nothing creates fences until the device's `CreateFence` exists
  Closes when: the device creates its fences through it and a title's frame pacing runs on it
- [CommandQueue](../../../core/libs/xdll/d3d12_x/objects/CommandQueue.cpp) (d3d12_x) - `ID3D12CommandQueue` signals, waits and executes command lists on the device's single Vulkan queue, so direct, compute and copy queues run serialized. A wait for a fence value nothing has promised yet holds that queue's later work, executed lists included, on the CPU until a GPU or CPU signal promises it, as D3D12 does, so a wait recorded before its signal does not deadlock the shared Vulkan queue. Lists AnyBox did not create, lists still recording and lists of another type stop the title; tile mappings and `GetClockCalibration` stop the title; queue priority is ignored
  Closes when: a title's frames execute through it, and async compute and copy queues get their own Vulkan queues where the GPU has them

## add Silent stubs

- [CommandQueue](../../../core/libs/xdll/d3d12_x/objects/CommandQueue.cpp) (d3d12_x) - `SetMarker`, `BeginEvent` and `EndEvent` do nothing: they are debugger annotations with no effect on rendering
  Closes when: markers are forwarded to Vulkan debug labels for a GPU debugger
