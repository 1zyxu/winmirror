# WinMirror

**Ultra-Low Latency C++ Screen Mirror (PC &rarr; Mac)**

WinMirror captures your Windows PC display at 60+ FPS straight from GPU VRAM and streams it to your Mac over direct USB-C or local network with **deeply sub-millisecond to 5ms end-to-end latency** ("deeply no lag").

---

## ⚡ Why Zero Lag?

1. **DXGI Desktop Duplication (DirectX 11)**:
   Captures frames directly from GPU memory in `< 1ms` without CPU copies or GDI stalls. (Falls back to GDI if running headless/restricted).
2. **Hardware/SIMD WIC Encoder**:
   Uses Windows Imaging Component (WIC) to compress frames into lightweight intra-frame JPEG in `~2ms` (no GOP/B-frame buffer latency).
3. **Queue-less Zero-Buffer Streaming**:
   Sockets use `TCP_NODELAY = 1`. If a client socket is busy, stale frames are discarded immediately so you are always seeing real-time frames with zero drift.
4. **Auto-Updater**:
   Built-in GitHub release auto-updater via `winhttp.dll` (`1zyxu/winmirror`).

---

## 🚀 Quick Start

### 1. On Windows PC:
Run `winmirror.exe`:
```cmd
winmirror.exe
```
You can also customize options:
```cmd
winmirror.exe --port 8888 --fps 60 --quality 0.75
```
Options:
- `--port <number>`: Streaming port (default: `8888`)
- `--fps <30-120>`: Target FPS (default: `60`)
- `--quality <0.1-1.0>`: Image quality (default: `0.75`)
- `--no-update`: Skip checking GitHub releases on launch
- `--update-only`: Check and apply updates then exit

### 2. Connect via USB-C:
- Connect your USB-C cable between PC and Mac.
- On Windows, enable **USB Tethering / Internet Sharing** on the USB interface, OR both machines will communicate instantly over your shared local Wi-Fi / LAN router.
- `winmirror.exe` automatically prints all active IP addresses in the console.

### 3. View on Mac:

#### Option A: Native High-Performance App (Recommended)
Run the native Cocoa viewer in Terminal:
```bash
./winmirror_mac
```
*(Automatically auto-discovers PC via UDP beacon, or pass IP directly: `./winmirror_mac 192.168.0.x`)*
- Press **F** to toggle Fullscreen.

#### Option B: Zero-Install Browser View
Open Safari or Google Chrome and navigate to:
```
http://<PC-IP>:8888
```
Includes an interactive, dark-mode canvas viewer with real-time FPS counter, fullscreen toggle (`F`), and scaling controls.

---

## 🔄 Auto-Update via GitHub

When a new release tag (e.g. `v1.0.1`) is published to GitHub (`1zyxu/winmirror`), `winmirror.exe`:
1. Detects the new release automatically via the GitHub API.
2. Downloads `winmirror_update.exe`.
3. Seamlessly replaces itself and restarts with zero manual intervention.

---

## 🛠️ Building from Source

To build both `winmirror.exe` and `winmirror_mac` locally on macOS:
```bash
./build.sh all
```
Or build individually:
```bash
./build.sh win    # Builds winmirror.exe
./build.sh mac    # Builds winmirror_mac
```
