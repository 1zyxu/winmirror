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
        // COM was already initialized in another mode, continue
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

    // Set JPEG quality
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

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    pFrame->SetPixelFormat(&format);

    uint32_t stride = width * 4;
    uint32_t bufferSize = stride * height;

    hr = pFrame->WritePixels(height, stride, bufferSize, const_cast<BYTE*>(bgra));
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
