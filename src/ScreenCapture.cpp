#include "ScreenCapture.h"
#include <iostream>

ScreenCapture::ScreenCapture() = default;

ScreenCapture::~ScreenCapture() {
    cleanup();
}

bool ScreenCapture::init() {
    if (initDxgi()) {
        std::cout << "[ScreenCapture] Initialized DXGI Desktop Duplication (Ultra-Low Latency GPU Capture: "
                  << m_width << "x" << m_height << ")" << std::endl;
        m_useDxgi = true;
        return true;
    }

    std::cout << "[ScreenCapture] DXGI Duplication failed, falling back to GDI capture..." << std::endl;
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
    m_width = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
    m_height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

    hr = dxgiOutput1->DuplicateOutput(m_d3dDevice, &m_duplication);
    dxgiOutput1->Release();
    if (FAILED(hr)) { cleanupDxgi(); return false; }

    // Prepare staging texture
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = m_width;
    desc.Height = m_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = m_d3dDevice->CreateTexture2D(&desc, nullptr, &m_stagingTexture);
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
        // If DXGI failed due to display change or access lost, retry init
        if (!m_duplication) {
            initDxgi();
        }
        return false;
    }
    return captureGdi(outFrame);
}

bool ScreenCapture::captureDxgi(RawFrame& outFrame) {
    if (!m_duplication || !m_d3dContext || !m_stagingTexture) return false;

    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    IDXGIResource* desktopResource = nullptr;

    HRESULT hr = m_duplication->AcquireNextFrame(16, &frameInfo, &desktopResource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false; // Screen did not change within timeout
    }
    if (FAILED(hr)) {
        // Access lost (e.g. UAC popup, screen lock, resolution switch)
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

    m_d3dContext->CopyResource(m_stagingTexture, desktopImage);
    desktopImage->Release();
    m_duplication->ReleaseFrame();

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = m_d3dContext->Map(m_stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    outFrame.width = m_width;
    outFrame.height = m_height;
    outFrame.pitch = m_width * 4;

    size_t totalBytes = (size_t)m_width * m_height * 4;
    if (outFrame.bgra.size() != totalBytes) {
        outFrame.bgra.resize(totalBytes);
    }

    const uint8_t* srcRow = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* dstRow = outFrame.bgra.data();
    size_t rowBytes = (size_t)m_width * 4;

    for (uint32_t y = 0; y < m_height; ++y) {
        memcpy(dstRow, srcRow, rowBytes);
        srcRow += mapped.RowPitch;
        dstRow += rowBytes;
    }

    m_d3dContext->Unmap(m_stagingTexture, 0);
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
    bmi.bmiHeader.biHeight = -static_cast<LONG>(m_height); // Top-down
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

    size_t totalBytes = (size_t)m_width * m_height * 4;
    if (outFrame.bgra.size() != totalBytes) {
        outFrame.bgra.resize(totalBytes);
    }

    memcpy(outFrame.bgra.data(), m_gdiBits, totalBytes);
    return true;
}
