# Reaper Timecode Toolkit

CLAP plugin. LTC or DAW time in, Art-Net timecode out.

- 24, 25, 29.97 and 30 fps LTC on either channel
- Coasts over dropouts
- Falls back to the DAW playhead
- Mutes the LTC leg of a one-sided file
- Unicast or broadcast
- Timecode offset

Installers: Releases. Unsigned, so right-click > Open on macOS, More info > Run anyway on Windows.

## Parameters

| | Default | Stored |
|---|---|---|
| Art-Net to | | per machine |
| Latency (ms) | 0 | per machine |
| Source: Auto, LTC only, DAW only | Auto | per project |
| Coast (frames) | 30 | per project |
| Mute LTC | on | per project |
| Exclusive | on | per project |
| Offset (HH:MM:SS:FF) | off | per project |

Exclusive: only the instance that started last sends. Coast has no control in the window.

Settings: `~/Library/Application Support/Reaper Timecode Toolkit/` or `%APPDATA%\Reaper Timecode Toolkit\`.

## Notes

- Art-Net types: 0 film, 1 EBU, 2 DF, 3 SMPTE.
- DAW time uses the REAPER project frame rate and start offset. Other hosts get 30 fps.
- In REAPER set Prevent anticipative FX on the track.

## Build

    ./build.sh            # macOS
    ./build.sh win        # Windows, cross-compiled (brew install mingw-w64 makensis)
    ./build.sh dist       # installers
    build.ps1             # Windows, native MinGW-w64

Decoder ported from the Cockos JSFX "SMPTE LTC Reader/Meter". CLAP headers in `third_party/clap` (MIT).
