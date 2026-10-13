# Windows x64 plugin and receiver with MinGW-w64 g++ on PATH; runs the checks.
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force build\win | Out-Null
$f = @("-std=c++17","-O2","-Wall","-Wextra","-Wno-unused-parameter","-D_WIN32_WINNT=0x0A00","-static","-s","-I","src","-I","third_party/clap/include")
& g++ @f -shared -o build\win\ReaperTimecodeToolkit.clap src\plugin.cpp src\gui_win.cpp -lws2_32 -liphlpapi -lwinmm -lavrt -lgdi32 -luser32 -lcomctl32
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& g++ @f -o build\win\test_core.exe test\test_core.cpp -lws2_32 -liphlpapi -lwinmm -lavrt
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& build\win\test_core.exe "$PSScriptRoot\build\win\ReaperTimecodeToolkit.clap"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$r = @("-std=c++17","-O2","-Wall","-Wextra","-Wno-unused-parameter","-D_WIN32_WINNT=0x0A00","-static","-s","-I","src")
& g++ @r -mwindows -o build\win\ArtNetTimecodeReceiver.exe receiver\app_win.cpp receiver\io_win.cpp -lws2_32 -liphlpapi -lwinmm -lavrt -lgdi32 -luser32 -lcomctl32 -lole32
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& g++ @r -o build\win\test_receiver.exe test\test_receiver.cpp -lws2_32 -liphlpapi -lwinmm -lavrt
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& build\win\test_receiver.exe
exit $LASTEXITCODE
