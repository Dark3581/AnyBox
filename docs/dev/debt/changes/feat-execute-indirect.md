# d3d12_x

## add Functional

- [IndirectCommands](../../../core/libs/xdll/d3d12_x/commands/IndirectCommands.cpp) (d3d12_x) - a command signature whose only argument is a draw, indexed draw or dispatch is executed with Vulkan's indirect commands, whose argument layouts equal D3D12's; a count buffer uses `vkCmdDraw(Indexed)IndirectCount`, and without `multiDrawIndirect` the draws are recorded one by one. Signatures that also change constants, views or vertex and index buffers per command, mesh and ray dispatches, and indirect dispatches with a count buffer are refused; they need commands generated on the GPU
  Closes when: a title's `ExecuteIndirect` calls run through it, and the signatures it uses beyond plain draws and dispatches are emulated or seen unused
