# runtime

## add Functional

- [LoaderRedirect](../../../core/libs/runtime/src/LoaderRedirect.cpp) (runtime) - run-time loads of DLLs AnyBox serves are redirected by rewriting the `LoadLibrary*` and `GetModuleHandle*` import slots of title-side modules, matched by the address `kernel32` and `kernelbase` export. Not covered: delay-loaded imports (resolved after the patch), loader functions reached through `GetProcAddress`, `LdrLoadDll` called directly, and loads made from inside Windows modules on a title's behalf. Data-file and resource loads keep the requested name. A routed name always gets the library beside `ab_runtime.dll`, even when the title passes a full path
  Closes when: a title's run-time loads are seen in its log to reach AnyBox's libraries, and delay-loaded loader imports are patched if a title has them
