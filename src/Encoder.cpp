#include "Encoder.h"
#include <iostream>

Encoder::Encoder() = default;

Encoder::~Encoder() {
    cleanup();
}

bool Encoder::init(float quality) {
    setQuality(quality);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        m_coInitialized = true;
    } else if (hr == RPC_E_CHANGED_MODE) {
        m_coInitialized = false;
    }

    hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IWICImagingFactory,
        reinterpret_cast<void**>(&m_factory)
    );

    if (FAILED(hr)) {
        std::cerr << "[Encoder] Failed to initialize WIC Imaging Factory. hr = " << std::hex << hr << std::dec << std::endl;
        return false;
    }

    return true;
}

void Encoder::cleanup() {
    if (m_factory) {
        m_factory->Release();
        m_factory = nullptr;
    }
    if (m_coInitialized) {
        CoUninitialize();
        m_coInitialized = false;
    }
}

bool Encoder::encodeBgraToJpeg(const uint8_t* bgra, uint32_t width, uint32_t height, std::vector<uint8_t>& outJpeg) {
    if (!m_factory || !bgra || width == 0 || height == 0) return false;

    // Convert 32-bit BGRA to 24-bit BGR (removes alpha channel so JPEG encoder receives exact 3 bytes/pixel)
    size_t totalPixels = static_cast<size_t>(width) * height;
    size_t bgrSize = totalPixels * 3;
    if (m_bgrBuffer.size() != bgrSize) {
        m_bgrBuffer.resize(bgrSize);
    }

    const uint8_t* src = bgra;
    uint8_t* dst = m_bgrBuffer.data();
    size_t i = 0;
    size_t fastPixels = totalPixels >= 4 ? totalPixels - 4 : 0;
    for (; i <= fastPixels; i += 4) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
        dst[3] = src[4]; dst[4] = src[5]; dst[5] = src[6];
        dst[6] = src[8]; dst[7] = src[9]; dst[8] = src[10];
        dst[9] = src[12]; dst[10] = src[13]; dst[11] = src[14];
        dst += 12;
        src += 16;
    }
    for (; i < totalPixels; ++i) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst += 3;
        src += 4;
    }

    IStream* pStream = nullptr;
    HRESULT hr = CreateStreamOnHGlobal(NULL, TRUE, &pStream);
    if (FAILED(hr)) return false;

    IWICBitmapEncoder* pEncoder = nullptr;
    hr = m_factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &pEncoder);
    if (FAILED(hr)) {
        pStream->Release();
        return false;
    }

    hr = pEncoder->Initialize(pStream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        pEncoder->Release();
        pStream->Release();
        return false;
    }

    IWICBitmapFrameEncode* pFrame = nullptr;
    IPropertyBag2* pPropertyBag = nullptr;
    hr = pEncoder->CreateNewFrame(&pFrame, &pPropertyBag);
    if (FAILED(hr)) {
        pEncoder->Release();
        pStream->Release();
        return false;
    }

    PROPBAG2 option = {};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT varValue;
    VariantInit(&varValue);
    varValue.vt = VT_R4;
    varValue.fltVal = m_quality;
    pPropertyBag->Write(1, &option, &varValue);

    hr = pFrame->Initialize(pPropertyBag);
    pPropertyBag->Release();
    if (FAILED(hr)) {
        pFrame->Release();
        pEncoder->Release();
        pStream->Release();
        return false;
    }

    pFrame->SetSize(width, height);

    // Exact matching format for JPEG encoder: 24bpp BGR (3 bytes per pixel)
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    pFrame->SetPixelFormat(&format);

    uint32_t stride = width * 3;
    uint32_t bufferSize = stride * height;

    hr = pFrame->WritePixels(height, stride, bufferSize, m_bgrBuffer.data());
    if (FAILED(hr)) {
        pFrame->Release();
        pEncoder->Release();
        pStream->Release();
        return false;
    }

    pFrame->Commit();
    pEncoder->Commit();

    pFrame->Release();
    pEncoder->Release();

    STATSTG stat;
    hr = pStream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr)) {
        pStream->Release();
        return false;
    }

    LARGE_INTEGER seekPos = {};
    pStream->Seek(seekPos, STREAM_SEEK_SET, NULL);

    outJpeg.resize(static_cast<size_t>(stat.cbSize.LowPart));
    ULONG bytesRead = 0;
    pStream->Read(outJpeg.data(), static_cast<ULONG>(outJpeg.size()), &bytesRead);

    pStream->Release();
    return bytesRead > 0;
}
