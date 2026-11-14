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

    echo "==> Building winmirror.exe (Windows x86_64 GUI + HEVC/H.264 200 Mbps HDR + Bluetooth)..."
    "$ZIG" c++ -target x86_64-windows-gnu -O3 -std=c++17 -Wl,-s -Wl,--subsystem,windows \
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
        -Wl,-sectcreate,__TEXT,__info_plist,mac/Info.plist \
        -framework Cocoa -framework QuartzCore -framework VideoToolbox \
        -framework AVFoundation -framework CoreMedia -framework CoreVideo \
        -framework AudioToolbox -framework CoreAudio -framework IOBluetooth \
        mac/main.mm -o winmirror_mac

    rm -rf WinMirror.app .dmg_stage .iconset_tmp WinMirror.dmg
    mkdir -p WinMirror.app/Contents/MacOS WinMirror.app/Contents/Resources .iconset_tmp/AppIcon.iconset
    cp winmirror_mac WinMirror.app/Contents/MacOS/winmirror_mac
    cp mac/Info.plist WinMirror.app/Contents/Info.plist
    chmod +x WinMirror.app/Contents/MacOS/winmirror_mac

    python3 - <<'PYEOF'
import struct, zlib
W = H = 512
rows = []
for y in range(H):
    row = bytearray([0])
    for x in range(W):
        dx = max(0, max(96 - x, x - 415))
        dy = max(0, max(96 - y, y - 415))
        if dx * dx + dy * dy > 96 * 96:
            row.extend((0, 0, 0, 0))
            continue
        r, g, b = (30, 88, 224) if y < 256 else (20, 62, 184)
        if 96 <= x <= 415 and 128 <= y <= 344 and (x < 114 or x > 397 or y < 146 or y > 326):
            r, g, b = 248, 250, 252
        elif 176 <= x <= 336 and 372 <= y <= 390:
            r, g, b = 147, 197, 253
        elif 216 <= x <= 316 and 186 <= y <= 286 and abs(y - 236) <= (316 - x) // 2:
            r, g, b = 52, 211, 153
        row.extend((r, g, b, 255))
    rows.append(bytes(row))
def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
ihdr = struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0)
idat = zlib.compress(b"".join(rows), 6)
with open(".iconset_tmp/base.png", "wb") as f:
    f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b""))
PYEOF
    sips -z 128 128 .iconset_tmp/base.png --out .iconset_tmp/AppIcon.iconset/icon_128x128.png >/dev/null
    sips -z 256 256 .iconset_tmp/base.png --out .iconset_tmp/AppIcon.iconset/icon_128x128@2x.png >/dev/null
    sips -z 256 256 .iconset_tmp/base.png --out .iconset_tmp/AppIcon.iconset/icon_256x256.png >/dev/null
    cp .iconset_tmp/base.png .iconset_tmp/AppIcon.iconset/icon_256x256@2x.png
    cp .iconset_tmp/base.png .iconset_tmp/AppIcon.iconset/icon_512x512.png
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
