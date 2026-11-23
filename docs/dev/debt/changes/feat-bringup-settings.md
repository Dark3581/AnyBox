# runtime

## add Functional

- [Settings](../../../core/libs/runtime/src/Settings.cpp) (runtime) - `AbxSetting` reads `anybox.cfg` beside the executable (or `ANYBOX_CONFIG`) once, on first use, with `ANYBOX_SET` overriding it: one lower-case `key = value` per line, `#` starting a comment, malformed and repeated lines logged and the last value kept. Values are text; each library parses and checks its own keys, and unknown keys are never reported because no list of keys exists
  Closes when: libraries register the keys they read, so a misspelled key in a title's file is reported
