# runtime technical debt

### Functional

- [Runtime](../../../core/libs/runtime/src/Runtime.cpp) (runtime) - `system.gpu` is always `null` until a graphics library reports the device
- [Runtime](../../../core/libs/runtime/src/Runtime.cpp) (runtime) - a C++ exception escaping an AnyBox library is reported as a crash with code `0x20474343`, without the exception message
- [TitleConfig](../../../core/libs/runtime/src/TitleConfig.cpp) (runtime) - the `MicrosoftGame.config` reader does not skip XML comments, so a `<TitleId>`, `<Identity>`, `<ShellVisuals>` or `<Executable>` inside a comment before the real element is read in its place
- [Runtime](../../../core/libs/runtime/src/Runtime.cpp) (runtime) - to keep the crash report when a C runtime or the title replaces the unhandled exception filter, a vectored handler puts the AnyBox filter back on top on every first-chance exception, not only fatal ones (C++ throws, `OutputDebugString`, thread naming, guard-page probes); a title's later `SetUnhandledExceptionFilter` call gets the AnyBox filter back as the previous one, and the title's own filter runs after the report is written
  Closes when: the crash report no longer needs the top-level filter
- [Runtime](../../../core/libs/runtime/src/Runtime.cpp) (runtime) - the guard against re-entering the AnyBox filter is process-wide, so a second thread that crashes while the first is inside the filter returns `EXCEPTION_CONTINUE_SEARCH` without a report and may end the process before the first report is complete; the mark that stops a second call in the same dispatch also holds one thread, so a crash on another thread in between can let the first thread's title filter run twice on a C runtime that calls the AnyBox filter itself
  Closes when: the guard is per thread
