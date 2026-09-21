# ltc-artnet

CLAP plugin, LTC or DAW time to Art-Net timecode. macOS (Cocoa) and Windows (Win32/GDI).

- Writing: `hq/writing-north-star.md`. Design: `hq/design-north-star.md`. Both bind here.
- `src/ltc_core.h` decoder, coaster, DAW clock, sender. `src/platform.h` clock, RT thread, sockets, prefs path.
  `src/plugin.cpp` CLAP glue. `src/plugin.h` shared state and `ui_state()` (all window text). `src/gui_*` per platform.
- Window text changes go in `ui_state()`, never in one GUI only.
- `./build.sh` must end with "all checks passed" before any commit. Timing tests need a quiet machine.
- Windows runtime is unverified. First checks on a Windows machine: `test_core.exe CT_LTC_ArtNet.clap`, then the
  installer, then REAPER: window scale at 100/150/200 %, Return in both fields, stepper hold and Shift-click,
  Source segments, state reload, packets at the desk. Record results in README "Status".
- State blob "CTLA" v3, 9 bytes. Parameter ids 1 Offset, 2 Source, 3 Coast: never renumber.
- Commits: author Chris Thoms, no AI attribution.
