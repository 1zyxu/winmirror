#pragma once

#include "Common.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

struct RawFrame {
    std::vector<uint8_t> bgra;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
};

class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();

    bool init();
    void cleanup();

    // Captures latest desktop frame onto double-buffered GPU texture or GDI fallback.
    // Returns true if a new desktop frame arrived.
    bool acquireFrame(uint32_t timeoutMs = 0);

    // Thread-safe AddRef snapshot of the newest captured GPU texture & owning device.
    bool grabGpuFrame(ID3D11Device** outDev, ID3D11DeviceContext** outCtx, ID3D11Texture2D** outTex,
                      uint32_t& outW, uint32_t& outH, DXGI_FORMAT& outFmt, bool& outHdr);

    // Polls OS hardware cursor cleanly (works even when desktop frame is static).
    // Returns true if cursor state or bitmap changed since last poll.
    bool pollCursor(CursorPayload& outMeta, std::vector<uint8_t>& outBgra, bool forceBitmap = false);

    // GDI / CPU readback fallback when GPU VideoProcessor isn't used
    bool copyCurrentFrameToBgra(RawFrame& outFrame);

    ID3D11Device* getD3DDevice() const { return m_d3dDevice; }
    ID3D11DeviceContext* getD3DContext() const { return m_d3dContext; }
    ID3D11Texture2D* getGpuTexture() const { return m_gpuCopyTex[m_latestSlot.load()]; }

    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    uint32_t getRefreshRateHz() const { return m_refreshRateHz; }
    bool isDxgiActive() const { return m_mode == CaptureMode::Dxgi || m_mode == CaptureMode::Wgc; }
    bool isWgcActive() const { return m_mode == CaptureMode::Wgc; }
    bool isHdrOutput() const { return m_isHdrOutput || m_format == DXGI_FORMAT_R16G16B16A16_FLOAT; }
    DXGI_FORMAT getPixelFormat() const { return m_format; }
    const char* getPixelFormatName() const;
    const std::string& getAdapterName() const { return m_adapterName; }
    bool isNvidiaGpu() const { return m_isNvidiaGpu; }

private:
    enum class CaptureMode { None, Dxgi, Wgc, Gdi };

    bool initGpuCapture();
    void cleanupGpu();
    bool createD3d11DeviceForAdapter(IDXGIAdapter1* adapter);
    bool ensureGpuCopyTextures(uint32_t w, uint32_t h, DXGI_FORMAT fmt);

    bool initDxgi(bool requireLiveOnly);
    bool initDxgiOnAdapter(IDXGIAdapter1* adapter, bool requireLivePresent, const RECT* primaryMonitorRect);
    bool acquireDxgi(uint32_t timeoutMs, bool requireLivePresent = false);
    bool seedGpuTextureFromGdi();

    bool initWgc();
    bool initWgcOnAdapter(IDXGIAdapter1* adapter, HMONITOR hMon, const RECT& monRect);
    void cleanupWgcInterfaces();
    bool acquireWgc();

    bool initGdi();
    void cleanupGdi();
    void cleanupGdiLocked();
    bool acquireGdi();

    static void initHalfTable();
    static void extractCursorBgra(HCURSOR hCursor, int cx, int cy, std::vector<uint8_t>& outPremulBgra);

    CaptureMode m_mode = CaptureMode::None;
    bool m_isHdrOutput = false;
    bool m_hasValidGpuFrame = false;
    uint32_t m_gpuReinitFails = 0;
    uint64_t m_lastGpuReinitMs = 0;
    uint64_t m_lastGdiRetryMs = 0;
    uint64_t m_lastGdiCaptureMs = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_refreshRateHz = 60;
    int32_t m_screenLeft = 0;
    int32_t m_screenTop = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_B8G8R8A8_UNORM;

    // Cursor tracking state
    POINT m_lastCursorPos = { -99999, -99999 };
    HCURSOR m_lastHCursor = nullptr;
    bool m_lastCursorVisible = false;
    std::vector<uint8_t> m_cachedCursorBgra;
    uint16_t m_cursorW = 32;
    uint16_t m_cursorH = 32;
    int16_t m_cursorHotX = 0;
    int16_t m_cursorHotY = 0;

    // GPU capture resources (double-buffered texture slot for lock-free Capture -> Encode overlap)
    std::mutex m_snapMutex;
    std::atomic<int> m_latestSlot{0};
    ID3D11Device* m_d3dDevice = nullptr;
    ID3D11DeviceContext* m_d3dContext = nullptr;
    IDXGIOutput6* m_dxgiOutput6 = nullptr;
    IDXGIOutputDuplication* m_duplication = nullptr;
    ID3D11Texture2D* m_gpuCopyTex[2] = { nullptr, nullptr };
    ID3D11Texture2D* m_stagingTexture = nullptr;
    std::string m_adapterName = "Default GPU";
    bool m_isNvidiaGpu = false;

    // Pure-COM Windows.Graphics.Capture session objects (opaque to keep header SDK-neutral)
    void* m_wgcD3dDevice = nullptr;
    void* m_wgcItem = nullptr;
    void* m_wgcPool = nullptr;
    void* m_wgcSession = nullptr;
    int32_t m_wgcPoolW = 0;
    int32_t m_wgcPoolH = 0;
    DXGI_FORMAT m_wgcPoolFmt = DXGI_FORMAT_B8G8R8A8_UNORM;

    // GDI fallback resources
    HDC m_hdcScreen = nullptr;
    HDC m_hdcMem = nullptr;
    HBITMAP m_hBitmap = nullptr;
    void* m_gdiBits = nullptr;
};
