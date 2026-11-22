# d3d12_x

## add Functional

- [TitleBufferWindow](../../../core/libs/xdll/d3d12_x/vulkan/TitleBufferWindow.cpp) (d3d12_x) - reserves and commits one range below 4 GB, imports it as a single Vulkan buffer with `VK_EXT_external_memory_host` and gives the heap base (device address minus window start) that translated shaders add to the console addresses they read. Nothing allocates from it yet: which title allocations belong in it (option A on [#4](https://github.com/Dark3581/AnyBox/issues/4)) comes from a title's XMem and buffer calls. The memory is pinned and read by the GPU over PCIe, and the largest free range below 4 GB limits its size (2 GiB in the probe process on the test PC, 3 GiB not available)
  Closes when: a title's buffers and descriptor heaps are allocated from it and its shaders read them
