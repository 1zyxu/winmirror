#pragma once

#include "Common.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <string>
#include <vector>

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

    // Captures latest desktop frame onto m_gpuCopyTexture (keeps VRAM on GPU) or GDI CPU fallback.
    // Returns true if a new desktop frame arrived.
    bool acquireFrame();

    // Polls OS hardware cursor cleanly (works even when desktop frame is static).
    // Returns true if cursor state or bitmap changed since last poll.
    bool pollCursor(CursorPayload& outMeta, std::vector<uint8_t>& outBgra, bool forceBitmap = false);

    // GDI / CPU readback fallback when GPU VideoProcessor isn't used
    bool copyCurrentFrameToBgra(RawFrame& outFrame);

    ID3D11Device* getD3DDevice() const { return m_d3dDevice; }
    ID3D11DeviceContext* getD3DContext() const { return m_d3dContext; }
    ID3D11Texture2D* getGpuTexture() const { return m_gpuCopyTex; }

    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    bool isDxgiActive() const { return m_useDxgi; }
    bool isHdrOutput() const { return m_isHdrOutput || m_format == DXGI_FORMAT_R16G16B16A16_FLOAT; }
    DXGI_FORMAT getPixelFormat() const { return m_format; }
    const std::string& getAdapterName() const { return m_adapterName; }
    bool isNvidiaGpu() const { return m_isNvidiaGpu; }

private:
    bool initDxgi();
    bool initDxgiOnAdapter(IDXGIAdapter1* adapter);
    void cleanupDxgi();
    bool acquireDxgi();

    bool initGdi();
    void cleanupGdi();
    bool acquireGdi();

    static void initHalfTable();
    static void extractCursorBgra(HCURSOR hCursor, int cx, int cy, std::vector<uint8_t>& outPremulBgra);

    bool m_useDxgi = true;
    bool m_isHdrOutput = false;
    bool m_hasNewGdiFrame = false;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
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

    // DXGI GPU resources
    ID3D11Device* m_d3dDevice = nullptr;
    ID3D11DeviceContext* m_d3dContext = nullptr;
    IDXGIOutputDuplication* m_duplication = nullptr;
    ID3D11Texture2D* m_gpuCopyTex = nullptr;
    ID3D11Texture2D* m_stagingTexture = nullptr;
    std::string m_adapterName = "Default GPU";
    bool m_isNvidiaGpu = false;

    // GDI fallback resources
    HDC m_hdcScreen = nullptr;
    HDC m_hdcMem = nullptr;
    HBITMAP m_hBitmap = nullptr;
    void* m_gdiBits = nullptr;
};
