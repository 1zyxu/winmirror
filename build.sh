#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

ZIG="/Users/karnajitdey/Code/ddd/.tools/zig-aarch64-macos-0.14.1/zig"
if ! command -v "$ZIG" &> /dev/null && command -v zig &> /dev/null; then
    ZIG="zig"
fi

build_win() {
    echo "==> Building winmirror_rt.dll (Windows x86_64 MMCSS / High-Res Timer / Driver Runtime)..."
    "$ZIG" c++ -target x86_64-windows-gnu -shared -O3 -std=c++17 -fno-rtti -fno-ident -ffunction-sections -fdata-sections -Wl,-s -Wl,--gc-sections \
        src/WinMirrorRuntime.cpp \
        -luser32 -ladvapi32 -lwinmm -lws2_32 \
        -o winmirror_rt.dll
    rm -f winmirror_rt.pdb

    echo "==> Building winmirror.exe (Windows x86_64 GUI + HEVC/H.264 200 Mbps HDR + Bluetooth)..."
    "$ZIG" c++ -target x86_64-windows-gnu -O3 -std=c++17 -fno-rtti -fno-ident -ffunction-sections -fdata-sections -Wl,-s -Wl,--gc-sections -Wl,--subsystem,windows \
        src/main.cpp \
        src/GuiWindow.cpp \
        src/ScreenCapture.cpp \
        src/Encoder.cpp \
        src/AudioCapture.cpp \
        src/AirPlayServer.cpp \
        src/BluetoothBridge.cpp \
        src/Beacon.cpp \
        src/Updater.cpp \
        -ld3d11 -ldxgi -lmfplat -lmf -lmfuuid -lwmcodecdspuuid -lole32 -loleaut32 \
        -lgdi32 -luser32 -lshell32 -ladvapi32 -lws2_32 -lbthprops -lwinhttp -liphlpapi -lwinmm \
        -o winmirror.exe
    rm -f winmirror.pdb winmirror_rt.pdb WinMirrorRuntime.lib main.lib winmirror_rt.lib *.exp
    echo "==> Built winmirror_rt.dll + winmirror.exe successfully!"
    ls -lh winmirror_rt.dll winmirror.exe
}

build_mac() {
    echo "==> Building winmirror_mac + WinMirror.app + WinMirror.dmg (macOS arm64 VideoToolbox HDR + CoreAudio + IOBluetooth)..."
    clang++ -O3 -std=c++17 \
        -framework Cocoa -framework QuartzCore -framework VideoToolbox \
        -framework AVFoundation -framework CoreMedia -framework CoreVideo \
        -framework AudioToolbox -framework CoreAudio -framework IOBluetooth \
        mac/main.mm -o winmirror_mac

    rm -rf WinMirror.app .dmg_stage .iconset_tmp WinMirror.dmg
    mkdir -p WinMirror.app/Contents/MacOS WinMirror.app/Contents/Resources .iconset_tmp/AppIcon.iconset
    cp winmirror_mac WinMirror.app/Contents/MacOS/winmirror_mac
    cp mac/Info.plist WinMirror.app/Contents/Info.plist
    chmod +x WinMirror.app/Contents/MacOS/winmirror_mac

    for s in 16 32 128 256 512; do
        sips -z $s $s assets/app_icon_mac.png --out ".iconset_tmp/AppIcon.iconset/icon_${s}x${s}.png" >/dev/null
    done
    sips -z 32 32 assets/app_icon_mac.png --out .iconset_tmp/AppIcon.iconset/icon_16x16@2x.png >/dev/null
    sips -z 64 64 assets/app_icon_mac.png --out .iconset_tmp/AppIcon.iconset/icon_32x32@2x.png >/dev/null
    sips -z 256 256 assets/app_icon_mac.png --out .iconset_tmp/AppIcon.iconset/icon_128x128@2x.png >/dev/null
    sips -z 512 512 assets/app_icon_mac.png --out .iconset_tmp/AppIcon.iconset/icon_256x256@2x.png >/dev/null
    cp assets/app_icon_mac.png .iconset_tmp/AppIcon.iconset/icon_512x512@2x.png
    iconutil -c icns .iconset_tmp/AppIcon.iconset -o WinMirror.app/Contents/Resources/AppIcon.icns
    rm -rf .iconset_tmp

    codesign --force --deep --sign - WinMirror.app 2>/dev/null || true

    mkdir -p .dmg_stage
    cp -R WinMirror.app .dmg_stage/
    ln -s /Applications .dmg_stage/Applications
    hdiutil create -volname "WinMirror AirPlay" -srcfolder .dmg_stage -ov -format UDZO WinMirror.dmg >/dev/null
    rm -rf .dmg_stage

    echo "==> Built winmirror_mac + WinMirror.app + WinMirror.dmg successfully!"
    ls -lh winmirror_mac WinMirror.dmg
}

TARGET="${1:-all}"
case "$TARGET" in
    win)
        build_win
        ;;
    mac)
        build_mac
        ;;
    all)
        build_win
        build_mac
        ;;
    *)
        echo "Usage: $0 [win|mac|all]"
        exit 1
        ;;
esac
