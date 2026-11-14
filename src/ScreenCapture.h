#pragma once

#include "Common.h"
#include <d3d11.h>
#include <dxgi1_2.h>
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

    bool capture(RawFrame& outFrame);

    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    bool isDxgiActive() const { return m_useDxgi; }
    DXGI_FORMAT getPixelFormat() const { return m_format; }

private:
    bool initDxgi();
    void cleanupDxgi();
    bool captureDxgi(RawFrame& outFrame);

    bool initGdi();
    void cleanupGdi();
    bool captureGdi(RawFrame& outFrame);

    static void initHalfTable();
    static void drawCursor(uint8_t* bgra, uint32_t width, uint32_t height, int32_t screenLeft, int32_t screenTop);

    bool m_useDxgi = true;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    int32_t m_screenLeft = 0;
    int32_t m_screenTop = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_B8G8R8A8_UNORM;

    // Cache for 60 FPS smooth mouse tracking even on static screens
    std::vector<uint8_t> m_lastCleanBgra;
    POINT m_lastCursorPos = { -1, -1 };
    HCURSOR m_lastHCursor = nullptr;

    // DXGI resources
    ID3D11Device* m_d3dDevice = nullptr;
    ID3D11DeviceContext* m_d3dContext = nullptr;
    IDXGIOutputDuplication* m_duplication = nullptr;
    ID3D11Texture2D* m_stagingTexture = nullptr;

    // GDI fallback resources
    HDC m_hdcScreen = nullptr;
    HDC m_hdcMem = nullptr;
    HBITMAP m_hBitmap = nullptr;
    void* m_gdiBits = nullptr;
};
