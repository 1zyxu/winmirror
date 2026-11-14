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

    // Returns true if a new frame was captured, false if timeout (static screen) or error
    bool capture(RawFrame& outFrame);

    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    bool isDxgiActive() const { return m_useDxgi; }

private:
    bool initDxgi();
    void cleanupDxgi();
    bool captureDxgi(RawFrame& outFrame);

    bool initGdi();
    void cleanupGdi();
    bool captureGdi(RawFrame& outFrame);

    bool m_useDxgi = true;
    uint32_t m_width = 0;
    uint32_t m_height = 0;

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
