# Reaper Timecode Toolkit

CLAP plugin - generates Artnet Timecode either from LTC audio or DAW time.

- 24, 25, 29.97 and 30 fps LTC supported
- Coasts over dropouts
- Mutes LTC by default and sends non-LTC legs to both stereo channels.
- Unicast or broadcast Artnet Timecode

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

Exclusive: only the instance that started last sends.

Settings: `~/Library/Application Support/Reaper Timecode Toolkit/` or `%APPDATA%\Reaper Timecode Toolkit\`.

## Notes

- DAW time uses the REAPER project frame rate and start offset. Other hosts get 30 fps.
