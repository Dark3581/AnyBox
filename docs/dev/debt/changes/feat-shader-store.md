# d3d12_x

## add Functional

- [ShaderStore](../../../core/libs/xdll/d3d12_x/store/ShaderStore.cpp) (d3d12_x) - console shaders are looked up by the SHA-256 of the DXBC container in a store of modules translated ahead of time; that the bytes a title passes at pipeline creation equal the container as stored in its package is unverified, and nothing calls the store until pipeline creation exists
  Closes when: pipeline creation looks shaders up and a title's run shows its shaders found
