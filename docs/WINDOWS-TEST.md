# Windows verification

Status: not started. 1.6.1 Windows binaries were cross-compiled on macOS (mingw-w64 GCC 16.2, static) and have
never run. Record results at the bottom of this file and in README "Status". One concern per commit.

## Get the binaries

    gh release download v1.6.1 -R christhoms/ltc-artnet -D dist

The release is a draft: `gh` must be logged in as christhoms. Files: `CT-LTC-ArtNet-1.6.1-win64-setup.exe`,
`test_core-1.6.1-win64.exe`.

## 1. Tests

Extract the plugin without installing (7-Zip opens the NSIS setup), or install first and point at the installed file:

    dist\test_core-1.6.1-win64.exe "C:\Program Files\Common Files\CLAP\CT_LTC_ArtNet.clap"

Pass: last line `all checks passed`. The run binds UDP 127.0.0.1:16454 and never touches the saved preferences.
Sender and DAW-time timing checks fail at random on a loaded machine: rerun twice before treating one as a bug.
Expect the Windows timer to be the weak point (`wait_until_s` in `src/platform.h`): report the printed
"worst deviation" figures either way.

## 2. Installer

- Run the setup. Expect SmartScreen (unsigned): More info > Run anyway.
- Check `C:\Program Files\Common Files\CLAP\CT_LTC_ArtNet.clap` exists and Apps > Installed apps lists
  "CT LTC to Art-Net Timecode".
- Uninstall, check both are gone. Reinstall for step 3.

## 3. REAPER

Preferences > Plug-ins > CLAP > Re-scan. Add "CT LTC to Art-Net Timecode" to a track with a "Track L - LTC R" file.
On that track: Track performance options > Prevent anticipative FX.

| Check | Expect |
|---|---|
| Window at 100 / 150 / 200 % display scale | 440x238 scaled, nothing clipped, text sharp |
| Play | green timecode, status `30 \| LTC R` (rate per file), right output carries the left leg |
| Mute LTC off | both legs pass through; on again: LTC muted within 10 ms |
| IP field: type address, Return | accepted, focus leaves the field; invalid address turns red |
| Offset field: type value, Return; stepper click, hold, Shift-click | 1 ms per click, repeat on hold, one frame with Shift; host parameter follows |
| Source segments | Auto / LTC only / DAW only switch; host parameter follows |
| Space bar with a field focused | types into the field, does not start REAPER's transport |
| Packets | Wireshark `udp.port == 6454`: one ArtTimeCode per frame, even spacing; or a grandMA3 timecode slot follows |
| Save, close, reopen project | Source, Coast, Mute LTC restored; LTC leg muted from the first block |
| `%APPDATA%\CT LTC ArtNet\` | `destination.txt`, `offset_ms.txt` written |
| Remove the FX, close REAPER | no hang, no crash |

Known risks to look at first: keyboard focus inside REAPER's FX window (`edit_proc` in `src/gui_win.cpp`), DPI
scale (`gui_set_scale`), timer resolution, `AvSetMmThreadCharacteristics` availability.

## Building on Windows

`build.ps1` builds the plugin and `test_core.exe` with a MinGW-w64 g++ on PATH (WinLibs or MSYS2 UCRT64). Untested.
Installer: `winget install NSIS.NSIS`, then
`makensis /DVERSION=<v> /DOUT=<path>\setup.exe /DSRC=<path>\build\win installer\win.nsi`.
macOS remains the release build host (`./build.sh dist`).

## Results

(none yet)
