# CT LTC to Art-Net Timecode

CLAP plugin: SMPTE LTC or DAW time to Art-Net timecode (ArtTimeCode, UDP 6454), one destination IP.

- Decode LTC at 24, 25, 29.97 (DF and NDF) and 30 fps from either input channel, 0 to -55 dBFS
- Coast over damaged LTC
- Send the host playhead when the input carries no LTC
- Mute a one-sided LTC leg and route the programme leg to both outputs
- Offset the output by -500 to +500 ms

Installers: Releases.

## Install

| Platform | Installer | Plugin path |
|---|---|---|
| macOS 11+, universal | `CT-LTC-ArtNet-<version>-mac.pkg` | `/Library/Audio/Plug-Ins/CLAP/CT_LTC_ArtNet.clap` |
| Windows 10+ x64 | `CT-LTC-ArtNet-<version>-win64-setup.exe` | `C:\Program Files\Common Files\CLAP\CT_LTC_ArtNet.clap` |

Neither installer is code signed. macOS: right-click > Open. Windows: More info > Run anyway.

## Parameters

| Parameter | Range | Default | Stored |
|---|---|---|---|
| Art-Net to | IPv4 address, unicast or broadcast | none | per machine |
| Offset (ms) | -500 to 500; positive sends later | 0 | per machine |
| Source | Auto, LTC only, DAW only | Auto | per project |
| Coast (frames) | 0 to 150; 0 disables | 30 | per project |

Coast is a host parameter only. The offset stepper moves 1 ms, or one frame with Shift.

Per-machine settings: `~/Library/Application Support/CT LTC ArtNet/` or `%APPDATA%\CT LTC ArtNet\`.

## Behaviour

- Art-Net type: 0 film (24), 1 EBU (25), 2 DF (29.97 drop), 3 SMPTE (30 and 29.97 NDF).
- DAW time follows the project frame rate and start offset in REAPER; other hosts send 30 fps. 23.976 sends as 24; 48, 50, 59.94 and 60 send at half rate.
- Auto falls back to DAW time 0.8 s after the last LTC frame, 0.15 s on an input with unknown signal, at once on a silent input.
- A negative offset overshoots by that amount at a stop.

## REAPER

Anticipative FX renders the track about 200 ms ahead of playback, so timecode leads the audio. On the LTC track:
Track performance options > Prevent anticipative FX.

## Build

    ./build.sh            # macOS plugin + tests
    ./build.sh install    # copy to ~/Library/Audio/Plug-Ins/CLAP
    ./build.sh win        # Windows x64 plugin + test_core.exe, cross-compiled
    ./build.sh dist       # both, plus installers in dist/

Requires Xcode command line tools; for Windows targets `brew install mingw-w64 makensis`.

    build/mac/test_core <plugin binary>
    test_core.exe CT_LTC_ArtNet.clap
    ffmpeg -v error -i <file> -af "pan=mono|c0=c1" -f f32le -ar 48000 - | build/mac/ltc_file_check 48000 30

## Status

macOS: 1.4.0 used in REAPER 7; 1.5.0 passes the test suite. Windows: compiles; not yet run.

LTC decoder ported from the Cockos JSFX "SMPTE LTC Reader/Meter". CLAP headers: `third_party/clap` (MIT).
