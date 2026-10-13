#!/bin/sh
# ./build.sh          macOS plugin and receiver app
# ./build.sh install  ... copied to ~/Library/Audio/Plug-Ins/CLAP
# ./build.sh win      Windows x64 plugin and receiver (cross-compiled, mingw-w64)
# ./build.sh dist     both, plus installers in dist/ (pkgbuild, makensis; makensis 3.12 needs a UTF-8 locale).
#                     The .pkg is Developer ID signed and notarized: needs both Developer ID certs in the keychain and
#                     a notarytool profile (xcrun notarytool store-credentials "$NOTARY_PROFILE" --apple-id ... --team-id SWS95WXK99)
set -e
cd "$(dirname "$0")"
V=$(sed -n 's/#define RTT_VERSION "\(.*\)"/\1/p' src/plugin.h)
INC="-I src -I third_party/clap/include"
SIGN_APP="${SIGN_APP:-Developer ID Application: Christopher Thoms (SWS95WXK99)}"
SIGN_PKG="${SIGN_PKG:-Developer ID Installer: Christopher Thoms (SWS95WXK99)}"
NOTARY_PROFILE="${NOTARY_PROFILE:-capture-ndi-region}"

mac() {
  B=build/mac/ReaperTimecodeToolkit.clap/Contents
  rm -rf build/mac && mkdir -p "$B/MacOS"
  cp src/Info.plist "$B/Info.plist"
  clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -fobjc-arc -fvisibility=hidden \
    -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 $INC -bundle -framework Cocoa \
    -o "$B/MacOS/ReaperTimecodeToolkit" src/plugin.cpp src/gui_mac.mm
  codesign --force -s - build/mac/ReaperTimecodeToolkit.clap
  clang++ -std=c++17 -O2 -Wall -Wextra $INC -o build/mac/test_core test/test_core.cpp
  clang++ -std=c++17 -O2 -fobjc-arc $INC -I test -framework Cocoa -o build/mac/gui_test test/gui_test.mm
  ./build/mac/test_core "$PWD/$B/MacOS/ReaperTimecodeToolkit"
  R="build/mac/Art-Net Timecode Receiver.app/Contents"
  mkdir -p "$R/MacOS"
  cp receiver/Info.plist "$R/Info.plist"
  clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -fobjc-arc -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 -I src \
    -framework Cocoa -framework CoreAudio -framework AudioUnit -framework AudioToolbox -framework CoreMIDI \
    -o "$R/MacOS/ArtNetTimecodeReceiver" receiver/app_mac.mm receiver/io_mac.mm
  codesign --force -s - "build/mac/Art-Net Timecode Receiver.app"
  clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -I src -o build/mac/test_receiver test/test_receiver.cpp
  ./build/mac/test_receiver
}

win() {
  rm -rf build/win && mkdir -p build/win
  W="x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -D_WIN32_WINNT=0x0A00 -static -s $INC"
  $W -shared -o build/win/ReaperTimecodeToolkit.clap src/plugin.cpp src/gui_win.cpp -lws2_32 -liphlpapi -lwinmm -lavrt -lgdi32 -luser32 -lcomctl32
  $W -o build/win/test_core.exe test/test_core.cpp -lws2_32 -liphlpapi -lwinmm -lavrt
  $W -mwindows -o build/win/ArtNetTimecodeReceiver.exe receiver/app_win.cpp receiver/io_win.cpp -lws2_32 -liphlpapi -lwinmm -lavrt -lgdi32 -luser32 -lcomctl32 -lole32
}

case "$1" in
  win) win ;;
  dist)
    mac; win
    security find-identity -v | grep -q "$SIGN_APP" || { echo "missing: $SIGN_APP"; exit 1; }
    security find-identity -v | grep -q "$SIGN_PKG" || { echo "missing: $SIGN_PKG"; exit 1; }
    xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" >/dev/null || { echo "missing notarytool profile: $NOTARY_PROFILE"; exit 1; }
    codesign --force --options runtime --timestamp -s "$SIGN_APP" build/mac/ReaperTimecodeToolkit.clap
    codesign --verify --strict --verbose=2 build/mac/ReaperTimecodeToolkit.clap
    codesign --force --options runtime --timestamp -s "$SIGN_APP" "build/mac/Art-Net Timecode Receiver.app"
    codesign --verify --strict --verbose=2 "build/mac/Art-Net Timecode Receiver.app"
    rm -rf dist build/pkgroot && mkdir -p dist "build/pkgroot/Library/Audio/Plug-Ins/CLAP" build/pkgroot/Applications
    cp -R build/mac/ReaperTimecodeToolkit.clap "build/pkgroot/Library/Audio/Plug-Ins/CLAP/"
    cp -R "build/mac/Art-Net Timecode Receiver.app" build/pkgroot/Applications/
    pkgbuild --analyze --root build/pkgroot build/component.plist >/dev/null
    plutil -replace 0.BundleIsRelocatable -bool NO build/component.plist  # else an existing copy elsewhere gets updated instead
    plutil -replace 1.BundleIsRelocatable -bool NO build/component.plist
    P="dist/ReaperTimecodeToolkit-$V-mac.pkg"
    pkgbuild --root build/pkgroot --component-plist build/component.plist --identifier uk.co.christhoms.reaper-timecode-toolkit --version "$V" --install-location / \
      --sign "$SIGN_PKG" --timestamp "$P"
    xcrun notarytool submit "$P" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$P"
    spctl -a -vv -t install "$P"
    LC_ALL=en_US.UTF-8 makensis -V2 -DVERSION="$V" -DOUT="$PWD/dist/ReaperTimecodeToolkit-$V-win64-setup.exe" -DSRC="$PWD/build/win" installer/win.nsi
    ls -l dist ;;
  install)
    mac
    rm -rf "$HOME/Library/Audio/Plug-Ins/CLAP/ReaperTimecodeToolkit.clap"
    mkdir -p "$HOME/Library/Audio/Plug-Ins/CLAP"
    cp -R build/mac/ReaperTimecodeToolkit.clap "$HOME/Library/Audio/Plug-Ins/CLAP/" ;;
  *) mac ;;
esac
