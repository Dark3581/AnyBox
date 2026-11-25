# gameinput

## remove [forwarded to the host's GameInput]

## add Functional

- [Objects](../../../core/libs/xdll/gameinput/Objects.cpp) (gameinput) - `GameInputCreate` serves its own v0 `IGameInput`, `IGameInputDevice` and `IGameInputReading` tables, ported from xcl's GameInput: the slot order and GUIDs are xcl's reading of the GDK `GameInput.h`, not checked against a title's calls here. There is one always-connected gamepad; readings are the state when asked, `GetNextReading` and `GetPreviousReading` find nothing, `GetTemporalReading` answers the current state, system-button and keyboard-layout callbacks get a token and are never called, and every view other than gamepad and UI navigation answers none. Slots xcl leaves open log once and return `0x80004001` in the whole return register, so a method declared `bool` reads it as true
  Closes when: a title's calls confirm the slots it uses, and the slots it reaches are answered from measured behaviour
- [Sources](../../../core/libs/xdll/gameinput/Sources.cpp) (gameinput) - the gamepad merges XInput pads 0 to 3 (buttons of every connected pad, sticks and triggers of the lowest connected slot), the keyboard while a window of this process has focus, `ANYBOX_PRESS_AT` / `ANYBOX_PRESS_A_AT` and the `\\.\pipe\anybox-gameinput` injection pipe. xcl's keyboard-and-mouse stick mapping (WASD, mouse, wheel) is not ported, rumble goes to the lowest connected slot only, empty XInput slots are searched once a second, and scripted press times count from process creation
  Closes when: the mouse and movement keys can play the sticks, and the press timer shares a clock with the log
