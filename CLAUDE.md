# reaper-timecode-toolkit

CLAP plugin, LTC or DAW time to Art-Net timecode. macOS (Cocoa) and Windows (Win32/GDI).

- Writing: `hq/writing-north-star.md`. Design: `hq/design-north-star.md`.
- `src/ltc_core.h` decoder, coaster, DAW clock, sender. `src/platform.h` clock, RT thread, sockets, interfaces, prefs.
  `src/plugin.cpp` CLAP glue. `src/plugin.h` shared state and `ui_state()`. `src/gui_*` per platform.
- Every visible word lives in `src/strings.h` (S), copy rules at its head. No new string without Chris's wording.
- `./build.sh` or `build.ps1` must end with `all checks passed` before a commit. Timing checks fail at random on a
  loaded machine: rerun first.
- State blob "CTLA" v6, 16 bytes (v1-v5 still load). Parameter ids 1 Latency, 2 Source, 3 Coast, 4 Mute LTC,
  5 Exclusive, 6 Offset: never renumber.
- Commits: author Chris Thoms, no AI attribution.
