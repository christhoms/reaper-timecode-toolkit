# Windows x64 plugin + tests with MinGW-w64 g++ on PATH.
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force build\win | Out-Null
$f = @("-std=c++17","-O2","-Wall","-Wextra","-Wno-unused-parameter","-D_WIN32_WINNT=0x0A00","-static","-s","-I","src","-I","third_party/clap/include")
& g++ @f -shared -o build\win\ReaperTimecodeToolkit.clap src\plugin.cpp src\gui_win.cpp -lws2_32 -liphlpapi -lwinmm -lavrt -lgdi32 -luser32 -lcomctl32
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& g++ @f -o build\win\test_core.exe test\test_core.cpp -lws2_32 -liphlpapi -lwinmm -lavrt
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& build\win\test_core.exe "$PSScriptRoot\build\win\ReaperTimecodeToolkit.clap"
exit $LASTEXITCODE
