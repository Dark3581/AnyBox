# runtime

## add Functional

- [Runtime](../../../core/libs/runtime/src/Runtime.cpp) (runtime) - stop and crash backtraces hold at most 32 frames as module and offset, without symbol names. A crash is unwound from the faulting context with the modules' unwind tables; a frame without unwind data is taken as a leaf whose return address is at the stack pointer, which can end the trace early or misread it in code that has neither unwind data nor a leaf's shape, such as hand-written assembly. `caller` skips AnyBox's `ab_*` modules and everything under the Windows directory, so a title DLL installed there would be skipped too
  Closes when: frames in AnyBox's own libraries carry function names, and a title's stop is seen traced through code without unwind data
- [Runtime](../../../core/libs/runtime/src/Runtime.cpp) (runtime) - `ANYBOX_WAIT_DEBUGGER` waits inside `ab_runtime.dll`'s `DllMain`, under the loader lock: a debugger's attach still works (the end-to-end test attaches one), but its own break-in thread runs only after the wait ends, and nothing else in the process can load a DLL meanwhile
  Closes when: the wait moves to a point after loader initialization that still comes before the title's entry point
