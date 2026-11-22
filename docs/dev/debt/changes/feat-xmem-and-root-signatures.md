# xmem

## add Functional

- [XMemAlloc](../../../core/libs/xdll/xmem/Memory.cpp) (xmem) - the attribute word is not decoded (no public header gives its layout; bundled middleware passes `0x5e50000` and a value read at run time), so alignment, memory type and protection requests are ignored: every block is zeroed read-write memory, 64-byte aligned below 64 KiB and from `VirtualAlloc` (64 KiB aligned) from 64 KiB up; a size of 0 stops with `AbxUnsupported`
  Closes when: the attribute layout is available from a public header or measured on a console
- [XMemFree](../../../core/libs/xdll/xmem/Memory.cpp) (xmem) - the attribute word is ignored; a null address or one that `XMemAlloc` did not return stops with `AbxUnsupported`
  Closes when: the attribute layout is known and console behaviour for a null address is measured
- [XMemVirtualQuery](../../../core/libs/xdll/xmem/Memory.cpp) (xmem) - fills the first 48 bytes of the 64-byte output from `VirtualQueryEx` as `MEMORY_BASIC_INFORMATION` (callers read `State` at +0x20 and `Protect` at +0x24, which match); bytes 0x30-0x3F are a console field that callers test (bits 0-1 of byte 0x30 when checking that memory is CPU-readable or GPU memory) and are left zero, so such checks fail; returns `TRUE` or `FALSE`, and whether the console returns a byte count instead is unknown (callers only test the low 32 bits for nonzero)
  Closes when: the layout of bytes 0x30-0x3F and the return value are sourced

## add Silent stubs

- [XMemGetWorkingSetStatistics](../../../core/libs/xdll/xmem/Memory.cpp) (xmem) - always returns `FALSE` and leaves the output untouched; callers pass `0x20000001` as the first argument and treat `FALSE` as no statistics (a used size of 0)
  Closes when: the first argument and the output layout are sourced

# d3d12_x

## change [every entry point stops the title]

- [Inventory](../../../core/libs/xdll/d3d12_x/Inventory.cpp) (d3d12_x) - `D3D12XboxCreateDevice`, `D3D12CreateDevice` and `D3D12GetDebugInterface` stop the title. D3D12.X interfaces have console-specific methods and layouts, so they cannot be forwarded to desktop `d3d12.dll`; the layer goes to Vulkan ([#4](https://github.com/Dark3581/AnyBox/issues/4)), with shaders from the shader store. `D3D12CreateDevice` and `D3D12GetDebugInterface` are declared with the desktop `d3d12.h` prototypes; the console prototypes are unverified
  Closes when: a title draws its first frame

## add Functional

- [RootSignature](../../../core/libs/xdll/d3d12_x/RootSignature.cpp) (d3d12_x) - the serializers and deserializers forward to the host `d3d12.dll`. The serializers clear flag bits 24-31 of a 1.0 or 1.1 description first, because console callers set `0x03000000` there and the host rejects any of those bits; what the bits mean on the console is unknown, and the serialized root signature no longer carries them. Another description version stops with `AbxUnsupported`. The deserializers forward unchanged, so a blob that carries those bits is rejected by the host with its own error
  Closes when: the console meaning of flag bits 24-31 is known and the device that consumes the blob accounts for it
