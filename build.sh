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
    "$ZIG" c++ -target x86_64-windows-gnu -shared -O3 -std=c++17 -Wl,-s \
        src/WinMirrorRuntime.cpp \
        -luser32 -ladvapi32 -lwinmm -lws2_32 \
        -o winmirror_rt.dll
    rm -f winmirror_rt.pdb

    echo "==> Building winmirror.exe (Windows x86_64 AirPlay HEVC/H.264 HDR + Bluetooth)..."
    "$ZIG" c++ -target x86_64-windows-gnu -O3 -std=c++17 -Wl,-s \
        src/main.cpp \
        src/ScreenCapture.cpp \
        src/Encoder.cpp \
        src/AudioCapture.cpp \
        src/AirPlayServer.cpp \
        src/BluetoothBridge.cpp \
        src/Beacon.cpp \
        src/Updater.cpp \
        -ld3d11 -ldxgi -lmfplat -lmf -lmfuuid -lwmcodecdspuuid -lole32 -loleaut32 \
        -lgdi32 -luser32 -ladvapi32 -lws2_32 -lbthprops -lwinhttp -liphlpapi -lwinmm \
        -o winmirror.exe
    rm -f winmirror.pdb winmirror_rt.pdb WinMirrorRuntime.lib main.lib winmirror_rt.lib *.exp
    echo "==> Built winmirror_rt.dll + winmirror.exe successfully!"
    ls -lh winmirror_rt.dll winmirror.exe
}

build_mac() {
    echo "==> Building winmirror_mac (macOS arm64 VideoToolbox HDR + CoreAudio + IOBluetooth)..."
    clang++ -O3 -std=c++17 \
        -Wl,-sectcreate,__TEXT,__info_plist,mac/Info.plist \
        -framework Cocoa -framework QuartzCore -framework VideoToolbox \
        -framework AVFoundation -framework CoreMedia -framework CoreVideo \
        -framework AudioToolbox -framework CoreAudio -framework IOBluetooth \
        mac/main.mm -o winmirror_mac
    echo "==> Built winmirror_mac successfully!"
    ls -lh winmirror_mac
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
