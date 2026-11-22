# d3d12_x

## add Functional

- [CommandRecorder](../../../core/libs/xdll/d3d12_x/commands/CommandRecorder.cpp) (d3d12_x) - records clears (render target and depth-stencil, whole or by rectangles, through dynamic rendering), buffer copies, copies between textures and placed footprints, texture copies and resolves, with layouts taken from the D3D12 state the caller passes. A new texture is moved from `UNDEFINED` to its initial state's layout without clearing it, although D3D12 zeroes committed resources unless told not to. Copies of textures with both depth and stencil planes, second planes, typed resolve formats and copies with a destination offset inside a footprint are not supported. Nothing records with it until the device's command lists exist
  Closes when: a title's command lists record through it and its frames show the copied and cleared data
- [CopyLayout](../../../core/libs/xdll/d3d12_x/commands/CopyLayout.cpp) (d3d12_x) - `DesktopCopyableFootprints` applies the desktop D3D12 rule (256-byte row pitch, 512-byte placement); whether the console's `GetCopyableFootprints` returns the same layouts is unknown, so the device must not answer a title with it until that is measured
  Closes when: the console footprint rule is measured or a title's footprints are seen to match it
