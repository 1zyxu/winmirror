#include "ScreenCapture.h"
#include <iostream>
#include <cmath>

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

typedef BOOL(WINAPI* PFN_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);

static uint8_t s_halfToByteTable[65536];
static bool s_tableInitialized = false;

struct HalfRGBA {
    uint16_t r;
    uint16_t g;
    uint16_t b;
    uint16_t a;
};

void ScreenCapture::initHalfTable() {
    if (s_tableInitialized) return;

    for (uint32_t i = 0; i < 65536; ++i) {
        uint16_t h = static_cast<uint16_t>(i);
        uint32_t sign = (h >> 15) & 1;
        uint32_t exp = (h >> 10) & 0x1f;
        uint32_t mant = h & 0x3ff;

        if (sign) {
            s_halfToByteTable[i] = 0;
            continue;
        }

        if (exp == 31) {
            s_halfToByteTable[i] = 255;
            continue;
        }

        float f = 0.0f;
        if (exp == 0) {
            f = (mant / 1024.0f) * std::ldexp(1.0f, -14);
        } else {
            f = (1.0f + mant / 1024.0f) * std::ldexp(1.0f, static_cast<int>(exp) - 15);
        }

        if (f <= 0.0f) {
            s_halfToByteTable[i] = 0;
        } else if (f >= 1.0f) {
            s_halfToByteTable[i] = 255;
        } else {
            // Standard sRGB gamma 2.2 curve
            float srgb = std::pow(f, 1.0f / 2.2f);
            s_halfToByteTable[i] = static_cast<uint8_t>(srgb * 255.0f + 0.5f);
        }
    }

    s_tableInitialized = true;
}

void ScreenCapture::drawCursor(uint8_t* bgra, uint32_t width, uint32_t height, int32_t screenLeft, int32_t screenTop) {
    CURSORINFO ci = { sizeof(CURSORINFO) };
    if (!GetCursorInfo(&ci)) return;
    if (!(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;

    ICONINFO ii = {};
    if (!GetIconInfo(ci.hCursor, &ii)) return;

    int cx = GetSystemMetrics(SM_CXCURSOR);
    int cy = GetSystemMetrics(SM_CYCURSOR);
    if (cx <= 0) cx = 32;
    if (cy <= 0) cy = 32;

    int cursorX = (ci.ptScreenPos.x - screenLeft) - ii.xHotspot;
    int cursorY = (ci.ptScreenPos.y - screenTop) - ii.yHotspot;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = cx;
    bmi.bmiHeader.biHeight = -cy; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hbmMem = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    HBITMAP hbmOld = static_cast<HBITMAP>(SelectObject(hdcMem, hbmMem));

    uint32_t* pPixels = reinterpret_cast<uint32_t*>(pBits);

    // 1. Copy real background from bgra underneath the cursor
    for (int y = 0; y < cy; ++y) {
        int sy = cursorY + y;
        for (int x = 0; x < cx; ++x) {
            int sx = cursorX + x;
            if (sy >= 0 && sy < static_cast<int>(height) && sx >= 0 && sx < static_cast<int>(width)) {
                size_t idx = (static_cast<size_t>(sy) * width + sx) * 4;
                pPixels[y * cx + x] = bgra[idx] | (bgra[idx + 1] << 8) | (bgra[idx + 2] << 16) | (0xFF << 24);
            } else {
                pPixels[y * cx + x] = 0;
            }
        }
    }

    // 2. Windows draws and blends the icon natively with full alpha/mask support
    DrawIconEx(hdcMem, 0, 0, ci.hCursor, cx, cy, 0, NULL, DI_NORMAL);

    // 3. Write blended pixels back to bgra buffer
    for (int y = 0; y < cy; ++y) {
        int sy = cursorY + y;
        if (sy < 0 || sy >= static_cast<int>(height)) continue;
        for (int x = 0; x < cx; ++x) {
            int sx = cursorX + x;
            if (sx < 0 || sx >= static_cast<int>(width)) continue;
            uint32_t c = pPixels[y * cx + x];
            size_t idx = (static_cast<size_t>(sy) * width + sx) * 4;
            bgra[idx + 0] = c & 0xFF;         // B
            bgra[idx + 1] = (c >> 8) & 0xFF;  // G
            bgra[idx + 2] = (c >> 16) & 0xFF; // R
        }
    }

    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbmMem);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
}

ScreenCapture::ScreenCapture() = default;

ScreenCapture::~ScreenCapture() {
    cleanup();
}

bool ScreenCapture::init() {
    initHalfTable();

    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        auto setDpiAwareness = (PFN_SetProcessDpiAwarenessContext)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (setDpiAwareness) {
            setDpiAwareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    if (initDxgi()) {
        std::cout << "[ScreenCapture] Initialized DXGI Desktop Duplication (GPU Capture: "
                  << m_width << "x" << m_height << ")" << std::endl;
        m_useDxgi = true;
        return true;
    }

    std::cout << "[ScreenCapture] Falling back to GDI capture..." << std::endl;
    if (initGdi()) {
        std::cout << "[ScreenCapture] Initialized GDI Capture (" << m_width << "x" << m_height << ")" << std::endl;
        m_useDxgi = false;
        return true;
    }

    return false;
}

void ScreenCapture::cleanup() {
    cleanupDxgi();
    cleanupGdi();
}

bool ScreenCapture::initDxgi() {
    cleanupDxgi();

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL chosenLevel;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        featureLevels,
        sizeof(featureLevels) / sizeof(featureLevels[0]),
        D3D11_SDK_VERSION,
        &m_d3dDevice,
        &chosenLevel,
        &m_d3dContext
    );

    if (FAILED(hr)) return false;

    IDXGIDevice* dxgiDevice = nullptr;
    hr = m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr)) { cleanupDxgi(); return false; }

    IDXGIAdapter* dxgiAdapter = nullptr;
    hr = dxgiDevice->GetParent(__uuidof(IDXGIAdapter), (void**)&dxgiAdapter);
    dxgiDevice->Release();
    if (FAILED(hr)) { cleanupDxgi(); return false; }

    IDXGIOutput* dxgiOutput = nullptr;
    hr = dxgiAdapter->EnumOutputs(0, &dxgiOutput);
    dxgiAdapter->Release();
    if (FAILED(hr)) { cleanupDxgi(); return false; }

    IDXGIOutput1* dxgiOutput1 = nullptr;
    hr = dxgiOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&dxgiOutput1);
    dxgiOutput->Release();
    if (FAILED(hr)) { cleanupDxgi(); return false; }

    DXGI_OUTPUT_DESC outputDesc;
    dxgiOutput1->GetDesc(&outputDesc);
    m_screenLeft = outputDesc.DesktopCoordinates.left;
    m_screenTop = outputDesc.DesktopCoordinates.top;
    m_width = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
    m_height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

    hr = dxgiOutput1->DuplicateOutput(m_d3dDevice, &m_duplication);
    dxgiOutput1->Release();
    if (FAILED(hr)) { cleanupDxgi(); return false; }

    return true;
}

void ScreenCapture::cleanupDxgi() {
    if (m_stagingTexture) { m_stagingTexture->Release(); m_stagingTexture = nullptr; }
    if (m_duplication) { m_duplication->Release(); m_duplication = nullptr; }
    if (m_d3dContext) { m_d3dContext->Release(); m_d3dContext = nullptr; }
    if (m_d3dDevice) { m_d3dDevice->Release(); m_d3dDevice = nullptr; }
}

bool ScreenCapture::capture(RawFrame& outFrame) {
    if (m_useDxgi) {
        if (captureDxgi(outFrame)) {
            return true;
        }
        if (!m_duplication) {
            initDxgi();
        }
        return false;
    }

    if (captureGdi(outFrame)) {
        drawCursor(outFrame.bgra.data(), outFrame.width, outFrame.height, 0, 0);
        return true;
    }
    return false;
}

bool ScreenCapture::captureDxgi(RawFrame& outFrame) {
    if (!m_duplication || !m_d3dContext) return false;

    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    IDXGIResource* desktopResource = nullptr;

    HRESULT hr = m_duplication->AcquireNextFrame(16, &frameInfo, &desktopResource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        CURSORINFO ci = { sizeof(CURSORINFO) };
        if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor) {
            if (ci.ptScreenPos.x != m_lastCursorPos.x || ci.ptScreenPos.y != m_lastCursorPos.y || ci.hCursor != m_lastHCursor) {
                if (!m_lastCleanBgra.empty()) {
                    m_lastCursorPos = ci.ptScreenPos;
                    m_lastHCursor = ci.hCursor;

                    outFrame.width = m_width;
                    outFrame.height = m_height;
                    outFrame.pitch = m_width * 4;
                    outFrame.bgra = m_lastCleanBgra;
                    drawCursor(outFrame.bgra.data(), m_width, m_height, m_screenLeft, m_screenTop);
                    return true;
                }
            }
        }
        return false;
    }
    if (FAILED(hr)) {
        cleanupDxgi();
        return false;
    }

    ID3D11Texture2D* desktopImage = nullptr;
    hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desktopImage);
    desktopResource->Release();
    if (FAILED(hr)) {
        m_duplication->ReleaseFrame();
        return false;
    }

    D3D11_TEXTURE2D_DESC actualDesc;
    desktopImage->GetDesc(&actualDesc);

    // Recreate staging texture if resolution or pixel format changed (e.g. 4K, 1080p, HDR Float16)
    if (!m_stagingTexture || m_width != actualDesc.Width || m_height != actualDesc.Height || m_format != actualDesc.Format) {
        if (m_stagingTexture) {
            m_stagingTexture->Release();
            m_stagingTexture = nullptr;
        }

        m_width = actualDesc.Width;
        m_height = actualDesc.Height;
        m_format = actualDesc.Format;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = m_width;
        desc.Height = m_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = m_format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        hr = m_d3dDevice->CreateTexture2D(&desc, nullptr, &m_stagingTexture);
        if (FAILED(hr)) {
            desktopImage->Release();
            m_duplication->ReleaseFrame();
            return false;
        }
    }

    m_d3dContext->CopyResource(m_stagingTexture, desktopImage);
    desktopImage->Release();
    m_duplication->ReleaseFrame();

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = m_d3dContext->Map(m_stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    outFrame.width = m_width;
    outFrame.height = m_height;
    outFrame.pitch = m_width * 4;

    size_t totalBytes = static_cast<size_t>(m_width) * m_height * 4;
    if (outFrame.bgra.size() != totalBytes) {
        outFrame.bgra.resize(totalBytes);
    }

    const uint8_t* srcRow = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* dstRow = outFrame.bgra.data();

    if (m_format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        // Windows 11 HDR (scRGB 64-bit float) -> 32-bit BGRA (True 1:1 pixel mapping)
        for (uint32_t y = 0; y < m_height; ++y) {
            const HalfRGBA* srcPixel = reinterpret_cast<const HalfRGBA*>(srcRow);
            uint8_t* dstPixel = dstRow;

            for (uint32_t x = 0; x < m_width; ++x) {
                dstPixel[0] = s_halfToByteTable[srcPixel[x].b];
                dstPixel[1] = s_halfToByteTable[srcPixel[x].g];
                dstPixel[2] = s_halfToByteTable[srcPixel[x].r];
                dstPixel[3] = 255;
                dstPixel += 4;
            }

            srcRow += mapped.RowPitch;
            dstRow += (m_width * 4);
        }
    } else if (m_format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        // 10-bit HDR -> 32-bit BGRA
        for (uint32_t y = 0; y < m_height; ++y) {
            const uint32_t* srcPixel = reinterpret_cast<const uint32_t*>(srcRow);
            uint8_t* dstPixel = dstRow;

            for (uint32_t x = 0; x < m_width; ++x) {
                uint32_t p = srcPixel[x];
                dstPixel[0] = static_cast<uint8_t>((p >> 20) & 0x3FF) >> 2;
                dstPixel[1] = static_cast<uint8_t>((p >> 10) & 0x3FF) >> 2;
                dstPixel[2] = static_cast<uint8_t>(p & 0x3FF) >> 2;
                dstPixel[3] = 255;
                dstPixel += 4;
            }

            srcRow += mapped.RowPitch;
            dstRow += (m_width * 4);
        }
    } else {
        // Standard SDR 8-bit B8G8R8A8_UNORM
        size_t rowBytes = static_cast<size_t>(m_width) * 4;
        for (uint32_t y = 0; y < m_height; ++y) {
            memcpy(dstRow, srcRow, rowBytes);
            srcRow += mapped.RowPitch;
            dstRow += rowBytes;
        }
    }

    m_d3dContext->Unmap(m_stagingTexture, 0);

    // Save clean desktop frame for smooth mouse interpolation
    m_lastCleanBgra = outFrame.bgra;

    CURSORINFO ci = { sizeof(CURSORINFO) };
    if (GetCursorInfo(&ci)) {
        m_lastCursorPos = ci.ptScreenPos;
        m_lastHCursor = ci.hCursor;
    }

    drawCursor(outFrame.bgra.data(), outFrame.width, outFrame.height, m_screenLeft, m_screenTop);
    return true;
}

bool ScreenCapture::initGdi() {
    cleanupGdi();

    m_width = GetSystemMetrics(SM_CXSCREEN);
    m_height = GetSystemMetrics(SM_CYSCREEN);

    m_hdcScreen = GetDC(NULL);
    if (!m_hdcScreen) return false;

    m_hdcMem = CreateCompatibleDC(m_hdcScreen);
    if (!m_hdcMem) { cleanupGdi(); return false; }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = m_width;
    bmi.bmiHeader.biHeight = -static_cast<LONG>(m_height);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    m_hBitmap = CreateDIBSection(m_hdcScreen, &bmi, DIB_RGB_COLORS, &m_gdiBits, NULL, 0);
    if (!m_hBitmap || !m_gdiBits) { cleanupGdi(); return false; }

    SelectObject(m_hdcMem, m_hBitmap);
    return true;
}

void ScreenCapture::cleanupGdi() {
    if (m_hBitmap) { DeleteObject(m_hBitmap); m_hBitmap = nullptr; }
    if (m_hdcMem) { DeleteDC(m_hdcMem); m_hdcMem = nullptr; }
    if (m_hdcScreen) { ReleaseDC(NULL, m_hdcScreen); m_hdcScreen = nullptr; }
    m_gdiBits = nullptr;
}

bool ScreenCapture::captureGdi(RawFrame& outFrame) {
    if (!m_hdcMem || !m_gdiBits) return false;

    if (!BitBlt(m_hdcMem, 0, 0, m_width, m_height, m_hdcScreen, 0, 0, SRCCOPY)) {
        return false;
    }

    outFrame.width = m_width;
    outFrame.height = m_height;
    outFrame.pitch = m_width * 4;

    size_t totalBytes = static_cast<size_t>(m_width) * m_height * 4;
    if (outFrame.bgra.size() != totalBytes) {
        outFrame.bgra.resize(totalBytes);
    }

    memcpy(outFrame.bgra.data(), m_gdiBits, totalBytes);
    return true;
}
