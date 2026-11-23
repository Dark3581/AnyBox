# d3d12_x

## add Functional

- [RootSignature](../../../core/libs/xdll/d3d12_x/objects/RootSignature.cpp) (d3d12_x) - `ID3D12RootSignature` holds the layout the host's `d3d12.dll` deserializes from the title's blob (parameters, root-word offsets, static samplers). Whether the console's serialized root signatures carry anything the desktop format lacks is unknown. Nothing creates root signatures until the device's `CreateRootSignature` exists
  Closes when: the device creates a title's root signatures through it
- [PipelineStateObject](../../../core/libs/xdll/d3d12_x/objects/PipelineObject.cpp) (d3d12_x) - `ID3D12PipelineState` for compute and graphics descriptions, with the pipeline from the pipeline library (cache first; the library owns the pipelines, so pipeline states must not outlive it) and a reference to the root signature. A description without a root signature is refused: reading a root signature embedded in the shader is not implemented. `GetCachedBlob` returns `E_NOTIMPL`, because a title's cached blobs hold console GPU code. Nothing creates pipeline states until the device's `CreateComputePipelineState` and `CreateGraphicsPipelineState` exist
  Closes when: the device creates a title's pipeline states through it, and embedded root signatures are read if titles use them
