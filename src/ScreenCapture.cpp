#include "ScreenCapture.h"
#include <iostream>
#include <cmath>
#include <algorithm>

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

        if (sign || exp == 31) {
            s_halfToByteTable[i] = sign ? 0 : 255;
            continue;
        }

        float f = (exp == 0)
            ? (mant / 1024.0f) * std::ldexp(1.0f, -14)
            : (1.0f + mant / 1024.0f) * std::ldexp(1.0f, static_cast<int>(exp) - 15);

        if (f <= 0.0f) {
            s_halfToByteTable[i] = 0;
        } else {
            // Smooth Reinhard tone mapping for CPU scRGB HDR fallback -> sRGB [0..255]
            float mapped = f / (1.0f + 0.15f * f);
            if (mapped > 1.0f) mapped = 1.0f;
            float srgb = std::pow(mapped, 1.0f / 2.2f);
            s_halfToByteTable[i] = static_cast<uint8_t>(std::min(255.0f, srgb * 255.0f + 0.5f));
        }
    }

    s_tableInitialized = true;
}

void ScreenCapture::extractCursorBgra(HCURSOR hCursor, int cx, int cy, std::vector<uint8_t>& outPremulBgra) {
    outPremulBgra.assign(static_cast<size_t>(cx) * cy * 4, 0);

    HDC hdcScreen = GetDC(NULL);
    HDC hdcBlack = CreateCompatibleDC(hdcScreen);
    HDC hdcWhite = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = cx;
    bmi.bmiHeader.biHeight = -cy; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    uint32_t* pBlack = nullptr;
    uint32_t* pWhite = nullptr;
    HBITMAP hbmBlack = CreateDIBSection(hdcBlack, &bmi, DIB_RGB_COLORS, reinterpret_cast<void**>(&pBlack), NULL, 0);
    HBITMAP hbmWhite = CreateDIBSection(hdcWhite, &bmi, DIB_RGB_COLORS, reinterpret_cast<void**>(&pWhite), NULL, 0);

    if (hbmBlack && hbmWhite && pBlack && pWhite) {
        HBITMAP oldB = static_cast<HBITMAP>(SelectObject(hdcBlack, hbmBlack));
        HBITMAP oldW = static_cast<HBITMAP>(SelectObject(hdcWhite, hbmWhite));

        const size_t nPix = static_cast<size_t>(cx) * cy;
        for (size_t i = 0; i < nPix; ++i) {
            pBlack[i] = 0x00000000;
            pWhite[i] = 0x00FFFFFF;
        }

        // Drawing onto pure black and pure white separates premultiplied color and true alpha/XOR mask cleanly
        DrawIconEx(hdcBlack, 0, 0, hCursor, cx, cy, 0, NULL, DI_NORMAL);
        DrawIconEx(hdcWhite, 0, 0, hCursor, cx, cy, 0, NULL, DI_NORMAL);

        for (size_t i = 0; i < nPix; ++i) {
            uint32_t b = pBlack[i];
            uint32_t w = pWhite[i];

            int bB = b & 0xFF, bG = (b >> 8) & 0xFF, bR = (b >> 16) & 0xFF;
            int wB = w & 0xFF, wG = (w >> 8) & 0xFF, wR = (w >> 16) & 0xFF;

            uint8_t outB = 0, outG = 0, outR = 0, outA = 0;
            if (wB < bB || wG < bG || wR < bR) {
                // XOR-inverted pixel (e.g. text I-beam): make visible white
                outB = 255; outG = 255; outR = 255; outA = 230;
            } else {
                int diff = ((wB - bB) + (wG - bG) + (wR - bR)) / 3;
                int a = std::max(0, std::min(255, 255 - diff));
                outA = static_cast<uint8_t>(a);
                outB = static_cast<uint8_t>(std::min(a, bB));
                outG = static_cast<uint8_t>(std::min(a, bG));
                outR = static_cast<uint8_t>(std::min(a, bR));
            }

            outPremulBgra[i * 4 + 0] = outB;
            outPremulBgra[i * 4 + 1] = outG;
            outPremulBgra[i * 4 + 2] = outR;
            outPremulBgra[i * 4 + 3] = outA;
        }

        SelectObject(hdcBlack, oldB);
        SelectObject(hdcWhite, oldW);
    }

    if (hbmBlack) DeleteObject(hbmBlack);
    if (hbmWhite) DeleteObject(hbmWhite);
    DeleteDC(hdcBlack);
    DeleteDC(hdcWhite);
    ReleaseDC(NULL, hdcScreen);
}

ScreenCapture::ScreenCapture() = default;

ScreenCapture::~ScreenCapture() {
    cleanup();
}

bool ScreenCapture::init() {
    initHalfTable();

    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32) {
        auto setDpiAwareness = reinterpret_cast<PFN_SetProcessDpiAwarenessContext>(
            GetProcAddress(hUser32, "SetProcessDpiAwarenessContext"));
        if (setDpiAwareness) {
            setDpiAwareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    if (initDxgi()) {
        std::cout << "[ScreenCapture] DXGI GPU Capture (" << m_width << "x" << m_height
                  << ", " << (isHdrOutput() ? "HDR16/10-bit" : "SDR 8-bit BGRA") << ")" << std::endl;
        m_useDxgi = true;
        return true;
    }

    std::cout << "[ScreenCapture] Falling back to GDI capture..." << std::endl;
    if (initGdi()) {
        std::cout << "[ScreenCapture] GDI Capture (" << m_width << "x" << m_height << ")" << std::endl;
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

    // Prefer NVIDIA adapter (0x10DE) that owns an active desktop output; fallback to primary GPU
    IDXGIAdapter1* preferredAdapter = nullptr;
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory) {
        IDXGIAdapter1* fallbackAdapter = nullptr;
        for (UINT idx = 0; ; ++idx) {
            IDXGIAdapter1* cand = nullptr;
            if (factory->EnumAdapters1(idx, &cand) == DXGI_ERROR_NOT_FOUND || !cand) break;

            DXGI_ADAPTER_DESC1 ad = {};
            cand->GetDesc1(&ad);
            if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                cand->Release();
                continue;
            }

            IDXGIOutput* out = nullptr;
            bool hasOutput = SUCCEEDED(cand->EnumOutputs(0, &out)) && out;
            if (out) out->Release();

            if (hasOutput) {
                if (ad.VendorId == 0x10DE && !preferredAdapter) {
                    preferredAdapter = cand;
                    break;
                }
                if (!fallbackAdapter) {
                    fallbackAdapter = cand;
                    continue;
                }
            }
            cand->Release();
        }
        if (!preferredAdapter) {
            preferredAdapter = fallbackAdapter;
        } else if (fallbackAdapter) {
            fallbackAdapter->Release();
        }
        factory->Release();
    }

    auto tryCreateOnAdapter = [&](IDXGIAdapter* adp, UINT flags) -> HRESULT {
        return D3D11CreateDevice(
            adp,
            adp ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &m_d3dDevice,
            &chosenLevel,
            &m_d3dContext
        );
    };

    HRESULT hr = tryCreateOnAdapter(preferredAdapter, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT);
    if (FAILED(hr)) {
        hr = tryCreateOnAdapter(preferredAdapter, D3D11_CREATE_DEVICE_BGRA_SUPPORT);
    }
    if (FAILED(hr) && preferredAdapter) {
        hr = tryCreateOnAdapter(nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT);
    }
    if (preferredAdapter) {
        preferredAdapter->Release();
        preferredAdapter = nullptr;
    }

    if (FAILED(hr) || !m_d3dDevice || !m_d3dContext) return false;

    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED(m_d3dDevice->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&mt)))) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }

    IDXGIDevice* dxgiDevice = nullptr;
    hr = m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr)) { cleanupDxgi(); return false; }
    dxgiDevice->SetGPUThreadPriority(7);

    IDXGIAdapter* dxgiAdapter = nullptr;
    hr = dxgiDevice->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&dxgiAdapter));
    dxgiDevice->Release();
    if (FAILED(hr)) { cleanupDxgi(); return false; }

    DXGI_ADAPTER_DESC desc = {};
    if (SUCCEEDED(dxgiAdapter->GetDesc(&desc))) {
        m_isNvidiaGpu = (desc.VendorId == 0x10DE);
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
        if (utf8Len > 1) {
            std::string name(utf8Len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, &name[0], utf8Len, nullptr, nullptr);
            m_adapterName = std::move(name);
        }
        std::cout << "[ScreenCapture] GPU Adapter: " << m_adapterName
                  << (m_isNvidiaGpu ? " (NVIDIA Dedicated)" : "") << std::endl;
    }

    IDXGIOutput* dxgiOutput = nullptr;
    hr = dxgiAdapter->EnumOutputs(0, &dxgiOutput);
    dxgiAdapter->Release();
    if (FAILED(hr)) { cleanupDxgi(); return false; }

    IDXGIOutput6* dxgiOutput6 = nullptr;
    if (SUCCEEDED(dxgiOutput->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&dxgiOutput6)))) {
        DXGI_OUTPUT_DESC1 desc1 = {};
        if (SUCCEEDED(dxgiOutput6->GetDesc1(&desc1))) {
            m_isHdrOutput = (desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        }
        dxgiOutput6->Release();
    }

    // Prefer DuplicateOutput1 so Windows hands us native HDR scRGB R16G16B16A16_FLOAT when Windows HDR is ON
    IDXGIOutput5* dxgiOutput5 = nullptr;
    if (SUCCEEDED(dxgiOutput->QueryInterface(__uuidof(IDXGIOutput5), reinterpret_cast<void**>(&dxgiOutput5)))) {
        DXGI_OUTPUT_DESC outputDesc = {};
        dxgiOutput5->GetDesc(&outputDesc);
        m_screenLeft = outputDesc.DesktopCoordinates.left;
        m_screenTop = outputDesc.DesktopCoordinates.top;
        m_width = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
        m_height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

        const DXGI_FORMAT supportedFormats[] = {
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            DXGI_FORMAT_R10G10B10A2_UNORM,
            DXGI_FORMAT_B8G8R8A8_UNORM
        };
        hr = dxgiOutput5->DuplicateOutput1(m_d3dDevice, 0, ARRAYSIZE(supportedFormats), supportedFormats, &m_duplication);
        dxgiOutput5->Release();
        dxgiOutput->Release();
        if (SUCCEEDED(hr) && m_duplication) {
            return true;
        }
        cleanupDxgi();
        return false;
    }

    IDXGIOutput1* dxgiOutput1 = nullptr;
    hr = dxgiOutput->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&dxgiOutput1));
    dxgiOutput->Release();
    if (FAILED(hr)) { cleanupDxgi(); return false; }

    DXGI_OUTPUT_DESC outputDesc = {};
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
    if (m_gpuCopyTex) { m_gpuCopyTex->Release(); m_gpuCopyTex = nullptr; }
    if (m_stagingTexture) { m_stagingTexture->Release(); m_stagingTexture = nullptr; }
    if (m_duplication) { m_duplication->Release(); m_duplication = nullptr; }
    if (m_d3dContext) { m_d3dContext->Release(); m_d3dContext = nullptr; }
    if (m_d3dDevice) { m_d3dDevice->Release(); m_d3dDevice = nullptr; }
}

bool ScreenCapture::acquireFrame() {
    if (m_useDxgi) {
        if (acquireDxgi()) return true;
        if (!m_duplication) {
            initDxgi();
        }
        return false;
    }
    return acquireGdi();
}

bool ScreenCapture::acquireDxgi() {
    if (!m_duplication || !m_d3dContext) return false;

    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
    IDXGIResource* desktopResource = nullptr;

    HRESULT hr = m_duplication->AcquireNextFrame(1, &frameInfo, &desktopResource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;
    }
    if (FAILED(hr)) {
        cleanupDxgi();
        return false;
    }

    // Check if frame contains actual screen image updates vs cursor-only update
    if (frameInfo.LastPresentTime.QuadPart == 0 && m_gpuCopyTex != nullptr) {
        desktopResource->Release();
        m_duplication->ReleaseFrame();
        return false;
    }

    ID3D11Texture2D* desktopImage = nullptr;
    hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&desktopImage));
    desktopResource->Release();
    if (FAILED(hr)) {
        m_duplication->ReleaseFrame();
        return false;
    }

    D3D11_TEXTURE2D_DESC actualDesc = {};
    desktopImage->GetDesc(&actualDesc);

    if (!m_gpuCopyTex || m_width != actualDesc.Width || m_height != actualDesc.Height || m_format != actualDesc.Format) {
        if (m_gpuCopyTex) { m_gpuCopyTex->Release(); m_gpuCopyTex = nullptr; }
        if (m_stagingTexture) { m_stagingTexture->Release(); m_stagingTexture = nullptr; }

        m_width = actualDesc.Width;
        m_height = actualDesc.Height;
        m_format = actualDesc.Format;

        D3D11_TEXTURE2D_DESC gpuDesc = {};
        gpuDesc.Width = m_width;
        gpuDesc.Height = m_height;
        gpuDesc.MipLevels = 1;
        gpuDesc.ArraySize = 1;
        gpuDesc.Format = m_format;
        gpuDesc.SampleDesc.Count = 1;
        gpuDesc.Usage = D3D11_USAGE_DEFAULT;
        gpuDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        hr = m_d3dDevice->CreateTexture2D(&gpuDesc, nullptr, &m_gpuCopyTex);
        if (FAILED(hr)) {
            desktopImage->Release();
            m_duplication->ReleaseFrame();
            return false;
        }
    }

    m_d3dContext->CopyResource(m_gpuCopyTex, desktopImage);
    desktopImage->Release();
    m_duplication->ReleaseFrame();
    return true;
}

bool ScreenCapture::copyCurrentFrameToBgra(RawFrame& outFrame) {
    if (!m_useDxgi) {
        if (!m_gdiBits || m_width == 0 || m_height == 0) return false;
        outFrame.width = m_width;
        outFrame.height = m_height;
        outFrame.pitch = m_width * 4;
        size_t totalBytes = static_cast<size_t>(m_width) * m_height * 4;
        if (outFrame.bgra.size() != totalBytes) outFrame.bgra.resize(totalBytes);
        memcpy(outFrame.bgra.data(), m_gdiBits, totalBytes);
        return true;
    }

    if (!m_gpuCopyTex || !m_d3dContext) return false;

    if (!m_stagingTexture) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = m_width;
        desc.Height = m_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = m_format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(m_d3dDevice->CreateTexture2D(&desc, nullptr, &m_stagingTexture))) {
            return false;
        }
    }

    m_d3dContext->CopyResource(m_stagingTexture, m_gpuCopyTex);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_d3dContext->Map(m_stagingTexture, 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }

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
        for (uint32_t y = 0; y < m_height; ++y) {
            const uint32_t* srcPixel = reinterpret_cast<const uint32_t*>(srcRow);
            uint8_t* dstPixel = dstRow;
            for (uint32_t x = 0; x < m_width; ++x) {
                uint32_t p = srcPixel[x];
                dstPixel[0] = static_cast<uint8_t>(((p >> 20) & 0x3FF) >> 2); // B
                dstPixel[1] = static_cast<uint8_t>(((p >> 10) & 0x3FF) >> 2); // G
                dstPixel[2] = static_cast<uint8_t>((p & 0x3FF) >> 2);         // R
                dstPixel[3] = 255;
                dstPixel += 4;
            }
            srcRow += mapped.RowPitch;
            dstRow += (m_width * 4);
        }
    } else if (m_format == DXGI_FORMAT_R8G8B8A8_UNORM || m_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        for (uint32_t y = 0; y < m_height; ++y) {
            const uint8_t* sp = srcRow;
            uint8_t* dp = dstRow;
            for (uint32_t x = 0; x < m_width; ++x) {
                dp[0] = sp[2]; // B
                dp[1] = sp[1]; // G
                dp[2] = sp[0]; // R
                dp[3] = 255;
                sp += 4;
                dp += 4;
            }
            srcRow += mapped.RowPitch;
            dstRow += (m_width * 4);
        }
    } else {
        size_t rowBytes = static_cast<size_t>(m_width) * 4;
        for (uint32_t y = 0; y < m_height; ++y) {
            memcpy(dstRow, srcRow, rowBytes);
            srcRow += mapped.RowPitch;
            dstRow += rowBytes;
        }
    }

    m_d3dContext->Unmap(m_stagingTexture, 0);
    return true;
}

bool ScreenCapture::pollCursor(CursorPayload& outMeta, std::vector<uint8_t>& outBgra, bool forceBitmap) {
    CURSORINFO ci = { sizeof(CURSORINFO) };
    if (!GetCursorInfo(&ci)) return false;

    bool visible = (ci.flags & CURSOR_SHOWING) && (ci.hCursor != nullptr);
    int relX = ci.ptScreenPos.x - m_screenLeft;
    int relY = ci.ptScreenPos.y - m_screenTop;

    bool shapeChanged = (ci.hCursor != m_lastHCursor) || (forceBitmap && m_cachedCursorBgra.empty());
    bool posChanged = (ci.ptScreenPos.x != m_lastCursorPos.x) ||
                      (ci.ptScreenPos.y != m_lastCursorPos.y) ||
                      (visible != m_lastCursorVisible);

    if (!forceBitmap && !shapeChanged && !posChanged) {
        return false;
    }

    if (visible && (shapeChanged || (forceBitmap && m_cachedCursorBgra.empty()))) {
        ICONINFO ii = {};
        if (GetIconInfo(ci.hCursor, &ii)) {
            m_cursorHotX = static_cast<int16_t>(ii.xHotspot);
            m_cursorHotY = static_cast<int16_t>(ii.yHotspot);
            int cx = GetSystemMetrics(SM_CXCURSOR);
            int cy = GetSystemMetrics(SM_CYCURSOR);
            if (cx <= 0 || cx > 128) cx = 32;
            if (cy <= 0 || cy > 128) cy = 32;
            m_cursorW = static_cast<uint16_t>(cx);
            m_cursorH = static_cast<uint16_t>(cy);
            extractCursorBgra(ci.hCursor, cx, cy, m_cachedCursorBgra);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
        }
    }

    m_lastCursorPos = ci.ptScreenPos;
    m_lastHCursor = ci.hCursor;
    m_lastCursorVisible = visible;

    outMeta.x = relX;
    outMeta.y = relY;
    outMeta.screenW = m_width;
    outMeta.screenH = m_height;
    outMeta.hotspotX = m_cursorHotX;
    outMeta.hotspotY = m_cursorHotY;
    outMeta.width = m_cursorW;
    outMeta.height = m_cursorH;
    outMeta.visible = visible ? 1 : 0;

    if ((shapeChanged || forceBitmap) && !m_cachedCursorBgra.empty()) {
        outMeta.hasBitmap = 1;
        outBgra = m_cachedCursorBgra;
    } else {
        outMeta.hasBitmap = 0;
        outBgra.clear();
    }

    return true;
}

bool ScreenCapture::initGdi() {
    cleanupGdi();

    m_width = GetSystemMetrics(SM_CXSCREEN);
    m_height = GetSystemMetrics(SM_CYSCREEN);
    m_screenLeft = 0;
    m_screenTop = 0;

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

bool ScreenCapture::acquireGdi() {
    if (!m_hdcMem || !m_gdiBits) return false;
    return BitBlt(m_hdcMem, 0, 0, m_width, m_height, m_hdcScreen, 0, 0, SRCCOPY) == TRUE;
}
