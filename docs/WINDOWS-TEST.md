# Windows verification

Status: first pass done 2026-09-21, see Results. 1.6.1 Windows binaries are cross-compiled on macOS (mingw-w64
GCC 16.2, static). Record results at the bottom of this file and in README "Status". One concern per commit.

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
| Window at 100 / 150 / 200 % display scale | 440x271 scaled, nothing clipped, text sharp |
| Play | green timecode, status `30 \| LTC R` (rate per file), right output carries the left leg |
| Mute LTC off | both legs pass through; on again: LTC muted within 10 ms |
| IP field: type address, Return | accepted, focus leaves the field; invalid address turns red |
| Offset field: type value, Return; stepper click, hold, Shift-click | 1 ms per click, repeat on hold, one frame with Shift; host parameter follows |
| Source segments | Auto / LTC only / DAW only switch; host parameter follows |
| Space bar with a field focused | types into the field, does not start REAPER's transport |
| Packets | Wireshark `udp.port == 6454`: one ArtTimeCode per frame, even spacing; or a grandMA3 timecode slot follows |
| Save, close, reopen project | Source, Coast, Mute LTC restored; LTC leg muted from the first block |
| `%APPDATA%\CT LTC ArtNet\` | `destination.txt`, `latency_ms.txt`, `latency_unit.txt` written |
| Remove the FX, close REAPER | no hang, no crash |

Known risks to look at first: keyboard focus inside REAPER's FX window (`edit_proc` in `src/gui_win.cpp`), DPI
scale (`gui_set_scale`), timer resolution, `AvSetMmThreadCharacteristics` availability.

## Building on Windows

`build.ps1` builds the plugin and `test_core.exe` with a MinGW-w64 g++ on PATH (WinLibs or MSYS2).
Installer: `winget install NSIS.NSIS`, then
`makensis /DVERSION=<v> /DOUT=<path>\setup.exe /DSRC=<path>\build\win installer\win.nsi`.
macOS remains the release build host (`./build.sh dist`).

## Results

### 2026-09-21, 1.6.1 release binaries (cross-compiled, hash B03AB8CF...), Windows 11 Pro 26200, REAPER 7.78

Machine: Ryzen X3D, 4K display at 150 %, REAPER on WaveOut 192 kHz / 1024 spls. Test file: generated 90 s
"tone L - 30 fps LTC R" from 01:00:00:00, confirmed with `ltc_file_check` (2697 frames, 0 filled in).

1. Tests: `all checks passed`, 3 runs of 3. Worst deviations per run:

| Check | Run 1 | Run 2 | Run 3 |
|---|---|---|---|
| real-time feed | 1.94 ms | 1.86 ms | 1.90 ms |
| offset +0.0 (mean / worst) | +1.30 / 2.77 ms | +1.20 / 2.33 ms | +1.48 / 2.75 ms |
| offset +20.0 (worst) | 0.55 ms | 0.52 ms | 0.56 ms |
| offset -50.0 (worst) | 0.57 ms | 0.75 ms | 0.60 ms |
| bursty feed | 1.85 ms | 2.27 ms | 2.63 ms |
| plugin 29.97 DF right channel | 1.90 ms | 1.80 ms | 1.84 ms |
| DAW time, no LTC | 2.10 ms | 2.56 ms | 2.59 ms |

   Offset +0.0 leaves about 1.3 ms late while +20 and -50 land within 0.6 ms. Read from the code, not measured:
   at offset 0 the frame reaches the sender at its own deadline, so the packet waits for the 2 ms ring poll in
   `Sender::loop` (mean 1 ms, worst 2 ms). Any other offset has the frame queued before the deadline. Not the timer.

2. Installer: pass, silent path only (`/S`). Install, uninstall, reinstall: plugin file, `uninstall.exe`, install
   folder and the Installed apps entry (name, 1.6.1, publisher) appear and go as expected; other plugins in
   `Common Files\CLAP` untouched. Not exercised: SmartScreen and the installer pages.

3. REAPER:

| Check | Result |
|---|---|
| Window at 150 % | pass: host window 660x357 (440x238 x 1.5), nothing clipped, text sharp. 100 % and 200 % not tested |
| Play | pass: green timecode, `30 \| LTC R`, right output carries the left leg, at 192 kHz |
| Mute LTC off / on | pass by track meters. The 10 ms figure not measured |
| IP field | pass: invalid address red, valid accepted and trimmed, focus leaves the field. Home, End, Shift-select, Delete work |
| IP field, Ctrl+A | FAIL: does not select all |
| IP field, first key after click | one character lost once, straight after dismissing another window's dialog; not reproduced |
| Offset field | pass: `12,5` accepted as 12.5; note shows `+0.93 fr` |
| Stepper | pass: 1 ms per click, Shift one frame (+33.3), hold repeats and stops on release; host parameter follows (30.83) |
| Source segments | pass: all three switch, host parameter follows; DAW only shows blue `DAW time \| 30 \| LTC R`; host-side parameter change redraws the window |
| Space bar with a field focused | pass: types a space, transport stays stopped; with no field focused it starts playback |
| Packets | pass, UDP listener on 127.0.0.1:6454 in place of Wireshark: 2698 packets over the 90 s file, one per frame, none skipped or repeated, type 3, mean 33.34 ms, worst deviation 2.2 ms before the offset was changed |
| Save, close, reopen | pass: Source LTC only, Coast 45, Mute LTC off, IP and offset restored. With Mute LTC on, a cold-load offline render has right = left from sample 0 |
| `%APPDATA%\CT LTC ArtNet\` | pass: `destination.txt` 127.0.0.1, `offset_ms.txt` 30.83 |
| Remove the FX while playing with its window open, close REAPER | pass: no hang, no crash event |

Not tested: grandMA3 following, display scale 100 % and 200 %, `AvSetMmThreadCharacteristics` result.

Open observations:

- Offline render sends Art-Net: each `reaper -renderproject` run put about 110 packets (01:00:00:03 to 01:00:03:23)
  on the wire at real-time spacing. Not Windows specific.
- One of four `reaper -renderproject` runs ended with a segmentation fault after writing the file: the run started a
  few seconds after the previous REAPER instance closed. No Windows error event, three repeats clean, a control
  project without the plugin clean once. Not attributed.
- `build.ps1` works unchanged with MSYS2 mingw64 g++ 13.2.0; the native build passes the suite with the same figures.
