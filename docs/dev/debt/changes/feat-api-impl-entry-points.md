# xgameruntime

## add Functional

- [ApiImpl](../../../core/libs/xdll/xgameruntime/ApiImpl.cpp) (xgameruntime) - GDK modules reach the runtime only through `QueryApiImpl`, `InitializeApiImpl`, `InitializeApiImplEx`, `InitializeApiImplEx2` and `UninitializeApiImpl`, then call the method tables `QueryApiImpl` returns; the named exports AnyBox implements (XTaskQueue, XAsync, XGameSave and the rest) are not what such titles call. The argument counts and types come from call sites in two bundled GDK modules (two GUID pointers and an output pointer for `QueryApiImpl`; two 32-bit values for `InitializeApiImpl`; two 64-bit values, flags and for `Ex2` a pointer for the `Ex` forms). Which GUID is the provider and which the interface is assumed, and the handshake values' meaning is unknown, so the four calls stop with their arguments and `UninitializeApiImpl` is a stub. `XErrorReport` logs its HRESULT and message
  Closes when: each interface's GUIDs and method slots are mapped from a public GDK thunk library or measured call sites, `QueryApiImpl` returns tables backed by the existing implementations, and the handshake's checks are known
