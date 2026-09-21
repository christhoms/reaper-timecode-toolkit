#!/bin/sh
# ./build.sh          macOS plugin + tests
# ./build.sh install  ... copied to ~/Library/Audio/Plug-Ins/CLAP
# ./build.sh win      Windows x64 plugin + test_core.exe (cross-compiled, mingw-w64)
# ./build.sh dist     both, plus installers in dist/ (pkgbuild, makensis; makensis 3.12 needs a UTF-8 locale)
set -e
cd "$(dirname "$0")"
V=$(sed -n 's/#define CTLTC_VERSION "\(.*\)"/\1/p' src/plugin.h)
INC="-I src -I third_party/clap/include"

mac() {
  B=build/mac/CT_LTC_ArtNet.clap/Contents
  rm -rf build/mac && mkdir -p "$B/MacOS"
  cp src/Info.plist "$B/Info.plist"
  clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -fobjc-arc -fvisibility=hidden \
    -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 $INC -bundle -framework Cocoa \
    -o "$B/MacOS/CT_LTC_ArtNet" src/plugin.cpp src/gui_mac.mm
  codesign --force -s - build/mac/CT_LTC_ArtNet.clap
  clang++ -std=c++17 -O2 -Wall -Wextra $INC -o build/mac/test_core test/test_core.cpp
  clang++ -std=c++17 -O2 -Wall -Wextra $INC -o build/mac/ltc_file_check test/ltc_file_check.cpp
  clang++ -std=c++17 -O2 -fobjc-arc $INC -I test -framework Cocoa -o build/mac/gui_test test/gui_test.mm
  ./build/mac/test_core "$PWD/$B/MacOS/CT_LTC_ArtNet"
}

win() {
  rm -rf build/win && mkdir -p build/win
  W="x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -D_WIN32_WINNT=0x0A00 -static -s $INC"
  $W -shared -o build/win/CT_LTC_ArtNet.clap src/plugin.cpp src/gui_win.cpp -lws2_32 -lwinmm -lavrt -lgdi32 -luser32 -lcomctl32
  $W -o build/win/test_core.exe test/test_core.cpp -lws2_32 -lwinmm -lavrt
  $W -o build/win/ltc_file_check.exe test/ltc_file_check.cpp -lws2_32 -lwinmm -lavrt
}

case "$1" in
  win) win ;;
  dist)
    mac; win
    rm -rf dist build/pkgroot && mkdir -p dist "build/pkgroot/Library/Audio/Plug-Ins/CLAP"
    cp -R build/mac/CT_LTC_ArtNet.clap "build/pkgroot/Library/Audio/Plug-Ins/CLAP/"
    pkgbuild --analyze --root build/pkgroot build/component.plist >/dev/null
    plutil -replace 0.BundleIsRelocatable -bool NO build/component.plist  # else an existing copy elsewhere gets updated instead
    pkgbuild --root build/pkgroot --component-plist build/component.plist --identifier uk.co.christhoms.ltc-artnet --version "$V" --install-location / \
      "dist/CT-LTC-ArtNet-$V-mac.pkg"
    LC_ALL=en_US.UTF-8 makensis -V2 -DVERSION="$V" -DOUT="$PWD/dist/CT-LTC-ArtNet-$V-win64-setup.exe" -DSRC="$PWD/build/win" installer/win.nsi
    cp build/win/test_core.exe "dist/test_core-$V-win64.exe"
    ls -l dist ;;
  install)
    mac
    rm -rf "$HOME/Library/Audio/Plug-Ins/CLAP/CT_LTC_ArtNet.clap"
    mkdir -p "$HOME/Library/Audio/Plug-Ins/CLAP"
    cp -R build/mac/CT_LTC_ArtNet.clap "$HOME/Library/Audio/Plug-Ins/CLAP/" ;;
  *) mac ;;
esac
