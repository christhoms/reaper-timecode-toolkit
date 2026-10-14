#!/bin/sh
# ./build.sh          macOS plugin, receiver app and Timecode Loopback driver
# ./build.sh install  ... copied to ~/Library/Audio/Plug-Ins/CLAP
# ./build.sh win      Windows x64 plugin and receiver (cross-compiled, mingw-w64)
# ./build.sh dist     both, plus installers in dist/ (pkgbuild, makensis; makensis 3.12 needs a UTF-8 locale):
#                     the receiver on its own, and the toolkit with the receiver as an option. On the Mac the receiver
#                     brings the Timecode Loopback audio device (/Library/Audio/Plug-Ins/HAL).
#                     The .pkg files are Developer ID signed and notarized: needs both Developer ID certs in the keychain and
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
  L=build/mac/TimecodeLoopback.driver/Contents
  mkdir -p "$L/MacOS"
  cp loopback/Info.plist "$L/Info.plist"
  clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 -bundle \
    -framework CoreAudio -framework CoreFoundation -o "$L/MacOS/TimecodeLoopback" loopback/loopback_mac.cpp
  codesign --force -s - build/mac/TimecodeLoopback.driver
  clang++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -I src -framework CoreAudio -framework CoreFoundation -o build/mac/test_loopback test/test_loopback.cpp
  ./build/mac/test_loopback "$PWD/$L/MacOS/TimecodeLoopback"
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
    for b in build/mac/ReaperTimecodeToolkit.clap "build/mac/Art-Net Timecode Receiver.app" build/mac/TimecodeLoopback.driver; do
      codesign --force --options runtime --timestamp -s "$SIGN_APP" "$b"
      codesign --verify --strict --verbose=2 "$b"
    done
    rm -rf dist build/root-plugin build/root-receiver build/root-loopback build/pkg
    mkdir -p dist build/pkg "build/root-plugin/Library/Audio/Plug-Ins/CLAP" build/root-receiver/Applications "build/root-loopback/Library/Audio/Plug-Ins/HAL"
    cp -R build/mac/ReaperTimecodeToolkit.clap "build/root-plugin/Library/Audio/Plug-Ins/CLAP/"
    cp -R "build/mac/Art-Net Timecode Receiver.app" build/root-receiver/Applications/
    cp -R build/mac/TimecodeLoopback.driver "build/root-loopback/Library/Audio/Plug-Ins/HAL/"
    # one component package each; BundleIsRelocatable off, else an existing copy elsewhere gets updated instead
    for c in plugin receiver loopback; do
      pkgbuild --analyze --root "build/root-$c" "build/$c.plist" >/dev/null
      plutil -replace 0.BundleIsRelocatable -bool NO "build/$c.plist"
    done
    pkgbuild --root build/root-plugin --component-plist build/plugin.plist --identifier uk.co.christhoms.reaper-timecode-toolkit --version "$V" --install-location / build/pkg/plugin.pkg
    pkgbuild --root build/root-receiver --component-plist build/receiver.plist --identifier uk.co.christhoms.artnet-timecode-receiver --version "$V" --install-location / build/pkg/receiver.pkg
    pkgbuild --root build/root-loopback --component-plist build/loopback.plist --scripts installer/mac-loopback --identifier uk.co.christhoms.timecode-loopback --version "$V" --install-location / build/pkg/loopback.pkg
    # the receiver alone, and the toolkit with the receiver as a choice (Customize); the loopback goes with the receiver
    RP="dist/ArtNetTimecodeReceiver-$V-mac.pkg"
    P="dist/ReaperTimecodeToolkit-$V-mac.pkg"
    productbuild --package build/pkg/receiver.pkg --package build/pkg/loopback.pkg --sign "$SIGN_PKG" --timestamp "$RP"
    # Customize: the toolkit fixed, the receiver unticked (as on Windows)
    cat > build/toolkit.dist <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="1">
  <options customize="allow" require-scripts="false" hostArchitectures="x86_64,arm64"/>
  <choices-outline>
    <line choice="plugin"/>
    <line choice="receiver"/>
  </choices-outline>
  <choice id="plugin" title="Reaper Timecode Toolkit" enabled="false">
    <pkg-ref id="uk.co.christhoms.reaper-timecode-toolkit"/>
  </choice>
  <choice id="receiver" title="Art-Net Timecode Receiver" start_selected="false">
    <pkg-ref id="uk.co.christhoms.artnet-timecode-receiver"/>
    <pkg-ref id="uk.co.christhoms.timecode-loopback"/>
  </choice>
  <pkg-ref id="uk.co.christhoms.reaper-timecode-toolkit" version="$V" onConclusion="none">plugin.pkg</pkg-ref>
  <pkg-ref id="uk.co.christhoms.artnet-timecode-receiver" version="$V" onConclusion="none">receiver.pkg</pkg-ref>
  <pkg-ref id="uk.co.christhoms.timecode-loopback" version="$V" onConclusion="none">loopback.pkg</pkg-ref>
</installer-gui-script>
EOF
    productbuild --distribution build/toolkit.dist --package-path build/pkg --sign "$SIGN_PKG" --timestamp "$P"
    for f in "$RP" "$P"; do
      xcrun notarytool submit "$f" --keychain-profile "$NOTARY_PROFILE" --wait
      xcrun stapler staple "$f"
      spctl -a -vv -t install "$f"
    done
    LC_ALL=en_US.UTF-8 makensis -V2 -DVERSION="$V" -DOUT="$PWD/dist/ArtNetTimecodeReceiver-$V-win64-setup.exe" -DSRC="$PWD/build/win" installer/receiver.nsi
    LC_ALL=en_US.UTF-8 makensis -V2 -DVERSION="$V" -DOUT="$PWD/dist/ReaperTimecodeToolkit-$V-win64-setup.exe" -DSRC="$PWD/build/win" \
      -DRXSETUP="$PWD/dist/ArtNetTimecodeReceiver-$V-win64-setup.exe" installer/win.nsi
    ls -l dist ;;
  install)
    mac
    rm -rf "$HOME/Library/Audio/Plug-Ins/CLAP/ReaperTimecodeToolkit.clap"
    mkdir -p "$HOME/Library/Audio/Plug-Ins/CLAP"
    cp -R build/mac/ReaperTimecodeToolkit.clap "$HOME/Library/Audio/Plug-Ins/CLAP/" ;;
  *) mac ;;
esac
