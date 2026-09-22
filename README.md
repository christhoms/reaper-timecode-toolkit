# Reaper Timecode Toolkit

CLAP plugin. SMPTE LTC or DAW time in, Art-Net timecode out.

- Decode LTC at 24, 25, 29.97 (DF and NDF) and 30 fps from either input channel, 0 to -55 dBFS
- Coast over damaged LTC
- Send the host playhead when the input carries no LTC
- Mute a one-sided LTC leg and route the programme leg to both outputs
- Send to one address or broadcast on one interface
- Add a timecode offset to the output

Installers: Releases.

## Install

| Platform | Installer | Plugin path |
|---|---|---|
| macOS 11+, universal | `ReaperTimecodeToolkit-<version>-mac.pkg` | `/Library/Audio/Plug-Ins/CLAP/ReaperTimecodeToolkit.clap` |
| Windows 10+ x64 | `ReaperTimecodeToolkit-<version>-win64-setup.exe` | `C:\Program Files\Common Files\CLAP\ReaperTimecodeToolkit.clap` |

Unsigned. macOS: right-click > Open. Windows: More info > Run anyway.

## Parameters

| Parameter | Range | Default | Stored |
|---|---|---|---|
| Art-Net to | IPv4 address, or Broadcast on an interface | none | per machine |
| Latency (ms) | -500 to 500 | 0 | per machine |
| Source | Auto, LTC only, DAW only | Auto | per project |
| Coast (frames) | 0 to 150 | 30 | per project |
| Mute LTC | on, off | on | per project |
| Exclusive | on, off | on | per project |
| Offset | on, off; HH:MM:SS:FF | off | per project |

Exclusive: the instance that started last sends, the others in the same host wait. Coast is a host parameter only.

Per-machine settings: `~/Library/Application Support/Reaper Timecode Toolkit/` or `%APPDATA%\Reaper Timecode Toolkit\`.

## Notes

- Art-Net type 0 film (24), 1 EBU (25), 2 DF (29.97 drop), 3 SMPTE (30, 29.97 NDF).
- DAW time follows the project frame rate and start offset in REAPER; other hosts send 30 fps. 23.976 sends as 24; 48, 50, 59.94 and 60 send at half rate.
- Auto falls back to DAW time 0.8 s after the last LTC frame.
- REAPER: on the LTC track, Track performance options > Prevent anticipative FX.

## Build

    ./build.sh            # macOS
    ./build.sh install    # copy to ~/Library/Audio/Plug-Ins/CLAP
    ./build.sh win        # Windows x64, cross-compiled
    ./build.sh dist       # both, installers in dist/
    build.ps1             # Windows, native MinGW-w64

Xcode command line tools; for Windows targets `brew install mingw-w64 makensis`.

LTC decoder ported from the Cockos JSFX "SMPTE LTC Reader/Meter". CLAP headers: `third_party/clap` (MIT).
