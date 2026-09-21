# ltc-artnet

CLAP plugin, LTC or DAW time to Art-Net timecode. macOS (Cocoa) and Windows (Win32/GDI).

- Writing: `hq/writing-north-star.md`. Design: `hq/design-north-star.md`. Both bind here.
- `src/ltc_core.h` decoder, coaster, DAW clock, sender. `src/platform.h` clock, RT thread, sockets, prefs path.
  `src/plugin.cpp` CLAP glue. `src/plugin.h` shared state and `ui_state()` (what the window shows). `src/gui_*` per platform.
- Every visible word lives in `src/strings.h` (S), with the copy rules at its head (MVR-FORGE S rules). GUIs and
  `plugin.cpp` only reference S. No copy outside it, no new string without Chris's wording or approval.
- `./build.sh` must end with "all checks passed" before any commit. Timing checks (sender, DAW time packets) fail at random on a loaded machine: rerun before suspecting the code.
- Windows runtime is unverified. First checks on a Windows machine: `test_core.exe CT_LTC_ArtNet.clap`, then the
  installer, then REAPER: window scale at 100/150/200 %, Return in both fields, stepper hold and Shift-click,
  Source segments, state reload, packets at the desk. Record results in README "Status".
- State blob "CTLA" v4, 10 bytes (v1-v3 still load). Parameter ids 1 Offset, 2 Source, 3 Coast, 4 Mute LTC: never renumber.
- Commits: author Chris Thoms, no AI attribution.
