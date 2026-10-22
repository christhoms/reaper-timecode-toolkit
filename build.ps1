# Windows x64 plugin and receiver with MinGW-w64 g++ on PATH; runs the checks.
param([string]$Out = "build\win")
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force $Out | Out-Null
$f = @("-std=c++17","-O2","-Wall","-Wextra","-Wno-unused-parameter","-D_WIN32_WINNT=0x0A00","-static","-s","-I","src","-I","third_party/clap/include")
& g++ @f -shared -o $Out\ReaperTimecodeToolkit.clap src\plugin.cpp src\gui_win.cpp receiver\io_win.cpp -lws2_32 -liphlpapi -lwinmm -lavrt -lgdi32 -luser32 -lcomctl32 -lole32
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& g++ @f -o $Out\test_core.exe test\test_core.cpp -lws2_32 -liphlpapi -lwinmm -lavrt
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $Out\test_core.exe (Resolve-Path "$Out\ReaperTimecodeToolkit.clap")
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$r = @("-std=c++17","-O2","-Wall","-Wextra","-Wno-unused-parameter","-D_WIN32_WINNT=0x0A00","-static","-s","-I","src")
& g++ @r -mwindows -o $Out\ArtNetTimecodeReceiver.exe receiver\app_win.cpp receiver\io_win.cpp -lws2_32 -liphlpapi -lwinmm -lavrt -lgdi32 -luser32 -lcomctl32 -lole32
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& g++ @r -o $Out\test_receiver.exe test\test_receiver.cpp -lws2_32 -liphlpapi -lwinmm -lavrt
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $Out\test_receiver.exe
exit $LASTEXITCODE
