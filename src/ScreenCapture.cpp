#include "ScreenCapture.h"
#include "CapturePolicy.h"
#include <hstring.h>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <cwchar>

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

typedef BOOL(WINAPI* PFN_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);

namespace {

uint32_t queryMonitorRefreshHz(const wchar_t* deviceName) {
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    const wchar_t* target = (deviceName && deviceName[0]) ? deviceName : nullptr;
    if (EnumDisplaySettingsExW(target, ENUM_CURRENT_SETTINGS, &dm, 0) && dm.dmDisplayFrequency > 1) {
        return static_cast<uint32_t>(dm.dmDisplayFrequency);
    }
    return 60u;
}

// Pure-COM WinRT WGC ABI definitions (verified against Windows SDK / Wine IDL) so builds need no cppwinrt
struct WgcSizeInt32 {
    int32_t Width;
    int32_t Height;
};
static_assert(sizeof(WgcSizeInt32) == 8, "WgcSizeInt32 must match WinRT ABI");

struct WgcClosable;
struct WgcClosableVtbl {
    HRESULT (STDMETHODCALLTYPE* QueryInterface)(WgcClosable*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE* AddRef)(WgcClosable*);
    ULONG   (STDMETHODCALLTYPE* Release)(WgcClosable*);
    HRESULT (STDMETHODCALLTYPE* GetIids)(WgcClosable*, ULONG*, IID**);
    HRESULT (STDMETHODCALLTYPE* GetRuntimeClassName)(WgcClosable*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE* GetTrustLevel)(WgcClosable*, int*);
    HRESULT (STDMETHODCALLTYPE* Close)(WgcClosable*);
};
struct WgcClosable { WgcClosableVtbl* lpVtbl; };

struct WgcCaptureItemInterop;
struct WgcCaptureItemInteropVtbl {
    HRESULT (STDMETHODCALLTYPE* QueryInterface)(WgcCaptureItemInterop*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE* AddRef)(WgcCaptureItemInterop*);
    ULONG   (STDMETHODCALLTYPE* Release)(WgcCaptureItemInterop*);
    HRESULT (STDMETHODCALLTYPE* CreateForWindow)(WgcCaptureItemInterop*, HWND, REFIID, void**);
    HRESULT (STDMETHODCALLTYPE* CreateForMonitor)(WgcCaptureItemInterop*, HMONITOR, REFIID, void**);
};
struct WgcCaptureItemInterop { WgcCaptureItemInteropVtbl* lpVtbl; };

struct WgcCaptureItem;
struct WgcCaptureItemVtbl {
    HRESULT (STDMETHODCALLTYPE* QueryInterface)(WgcCaptureItem*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE* AddRef)(WgcCaptureItem*);
    ULONG   (STDMETHODCALLTYPE* Release)(WgcCaptureItem*);
    HRESULT (STDMETHODCALLTYPE* GetIids)(WgcCaptureItem*, ULONG*, IID**);
    HRESULT (STDMETHODCALLTYPE* GetRuntimeClassName)(WgcCaptureItem*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE* GetTrustLevel)(WgcCaptureItem*, int*);
    HRESULT (STDMETHODCALLTYPE* get_DisplayName)(WgcCaptureItem*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE* get_Size)(WgcCaptureItem*, WgcSizeInt32*);
    HRESULT (STDMETHODCALLTYPE* add_Closed)(WgcCaptureItem*, void*, int64_t*);
    HRESULT (STDMETHODCALLTYPE* remove_Closed)(WgcCaptureItem*, int64_t);
};
struct WgcCaptureItem { WgcCaptureItemVtbl* lpVtbl; };

struct WgcCaptureFrame;
struct WgcCaptureFrameVtbl {
    HRESULT (STDMETHODCALLTYPE* QueryInterface)(WgcCaptureFrame*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE* AddRef)(WgcCaptureFrame*);
    ULONG   (STDMETHODCALLTYPE* Release)(WgcCaptureFrame*);
    HRESULT (STDMETHODCALLTYPE* GetIids)(WgcCaptureFrame*, ULONG*, IID**);
    HRESULT (STDMETHODCALLTYPE* GetRuntimeClassName)(WgcCaptureFrame*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE* GetTrustLevel)(WgcCaptureFrame*, int*);
    HRESULT (STDMETHODCALLTYPE* get_Surface)(WgcCaptureFrame*, IUnknown**);
    HRESULT (STDMETHODCALLTYPE* get_SystemRelativeTime)(WgcCaptureFrame*, int64_t*);
    HRESULT (STDMETHODCALLTYPE* get_ContentSize)(WgcCaptureFrame*, WgcSizeInt32*);
};
struct WgcCaptureFrame { WgcCaptureFrameVtbl* lpVtbl; };

struct WgcCaptureSession;
struct WgcCaptureSessionVtbl {
    HRESULT (STDMETHODCALLTYPE* QueryInterface)(WgcCaptureSession*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE* AddRef)(WgcCaptureSession*);
    ULONG   (STDMETHODCALLTYPE* Release)(WgcCaptureSession*);
    HRESULT (STDMETHODCALLTYPE* GetIids)(WgcCaptureSession*, ULONG*, IID**);
    HRESULT (STDMETHODCALLTYPE* GetRuntimeClassName)(WgcCaptureSession*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE* GetTrustLevel)(WgcCaptureSession*, int*);
    HRESULT (STDMETHODCALLTYPE* StartCapture)(WgcCaptureSession*);
};
struct WgcCaptureSession { WgcCaptureSessionVtbl* lpVtbl; };

struct WgcCaptureSession2;
struct WgcCaptureSession2Vtbl {
    HRESULT (STDMETHODCALLTYPE* QueryInterface)(WgcCaptureSession2*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE* AddRef)(WgcCaptureSession2*);
    ULONG   (STDMETHODCALLTYPE* Release)(WgcCaptureSession2*);
    HRESULT (STDMETHODCALLTYPE* GetIids)(WgcCaptureSession2*, ULONG*, IID**);
    HRESULT (STDMETHODCALLTYPE* GetRuntimeClassName)(WgcCaptureSession2*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE* GetTrustLevel)(WgcCaptureSession2*, int*);
    HRESULT (STDMETHODCALLTYPE* get_IsCursorCaptureEnabled)(WgcCaptureSession2*, uint8_t*);
    HRESULT (STDMETHODCALLTYPE* put_IsCursorCaptureEnabled)(WgcCaptureSession2*, uint8_t);
};
struct WgcCaptureSession2 { WgcCaptureSession2Vtbl* lpVtbl; };

struct WgcCaptureSession3;
struct WgcCaptureSession3Vtbl {
    HRESULT (STDMETHODCALLTYPE* QueryInterface)(WgcCaptureSession3*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE* AddRef)(WgcCaptureSession3*);
    ULONG   (STDMETHODCALLTYPE* Release)(WgcCaptureSession3*);
    HRESULT (STDMETHODCALLTYPE* GetIids)(WgcCaptureSession3*, ULONG*, IID**);
    HRESULT (STDMETHODCALLTYPE* GetRuntimeClassName)(WgcCaptureSession3*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE* GetTrustLevel)(WgcCaptureSession3*, int*);
    HRESULT (STDMETHODCALLTYPE* get_IsBorderRequired)(WgcCaptureSession3*, uint8_t*);
    HRESULT (STDMETHODCALLTYPE* put_IsBorderRequired)(WgcCaptureSession3*, uint8_t);
};
struct WgcCaptureSession3 { WgcCaptureSession3Vtbl* lpVtbl; };

struct WgcFramePool;
struct WgcFramePoolVtbl {
    HRESULT (STDMETHODCALLTYPE* QueryInterface)(WgcFramePool*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE* AddRef)(WgcFramePool*);
    ULONG   (STDMETHODCALLTYPE* Release)(WgcFramePool*);
    HRESULT (STDMETHODCALLTYPE* GetIids)(WgcFramePool*, ULONG*, IID**);
    HRESULT (STDMETHODCALLTYPE* GetRuntimeClassName)(WgcFramePool*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE* GetTrustLevel)(WgcFramePool*, int*);
    HRESULT (STDMETHODCALLTYPE* Recreate)(WgcFramePool*, IUnknown*, int32_t, int32_t, WgcSizeInt32);
    HRESULT (STDMETHODCALLTYPE* TryGetNextFrame)(WgcFramePool*, WgcCaptureFrame**);
    HRESULT (STDMETHODCALLTYPE* add_FrameArrived)(WgcFramePool*, void*, int64_t*);
    HRESULT (STDMETHODCALLTYPE* remove_FrameArrived)(WgcFramePool*, int64_t);
    HRESULT (STDMETHODCALLTYPE* CreateCaptureSession)(WgcFramePool*, WgcCaptureItem*, WgcCaptureSession**);
    HRESULT (STDMETHODCALLTYPE* get_DispatcherQueue)(WgcFramePool*, void**);
};
struct WgcFramePool { WgcFramePoolVtbl* lpVtbl; };

struct WgcFramePoolStatics;
struct WgcFramePoolStaticsVtbl {
    HRESULT (STDMETHODCALLTYPE* QueryInterface)(WgcFramePoolStatics*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE* AddRef)(WgcFramePoolStatics*);
    ULONG   (STDMETHODCALLTYPE* Release)(WgcFramePoolStatics*);
    HRESULT (STDMETHODCALLTYPE* GetIids)(WgcFramePoolStatics*, ULONG*, IID**);
    HRESULT (STDMETHODCALLTYPE* GetRuntimeClassName)(WgcFramePoolStatics*, HSTRING*);
    HRESULT (STDMETHODCALLTYPE* GetTrustLevel)(WgcFramePoolStatics*, int*);
    HRESULT (STDMETHODCALLTYPE* Create)(WgcFramePoolStatics*, IUnknown*, int32_t, int32_t, WgcSizeInt32, WgcFramePool**);
};
struct WgcFramePoolStatics { WgcFramePoolStaticsVtbl* lpVtbl; };

struct WgcDxgiInterfaceAccess;
struct WgcDxgiInterfaceAccessVtbl {
    HRESULT (STDMETHODCALLTYPE* QueryInterface)(WgcDxgiInterfaceAccess*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE* AddRef)(WgcDxgiInterfaceAccess*);
    ULONG   (STDMETHODCALLTYPE* Release)(WgcDxgiInterfaceAccess*);
    HRESULT (STDMETHODCALLTYPE* GetInterface)(WgcDxgiInterfaceAccess*, REFIID, void**);
};
struct WgcDxgiInterfaceAccess { WgcDxgiInterfaceAccessVtbl* lpVtbl; };

const GUID kIID_IClosable = { 0x30d5a829, 0x7fa4, 0x4026, { 0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e } };
const GUID kIID_IGraphicsCaptureItemInterop = { 0x3628e81b, 0x3cac, 0x4c60, { 0xb7, 0xf4, 0x23, 0xce, 0x0e, 0x0c, 0x33, 0x56 } };
const GUID kIID_IGraphicsCaptureItem = { 0x79c3f95b, 0x31f7, 0x4ec2, { 0xa4, 0x64, 0x63, 0x2e, 0xf5, 0xd3, 0x07, 0x60 } };
const GUID kIID_IDirect3D11CaptureFramePoolStatics2 = { 0x589b103f, 0x6bbc, 0x5df5, { 0xa9, 0x91, 0x02, 0xe2, 0x8b, 0x3b, 0x66, 0xd5 } };
const GUID kIID_IGraphicsCaptureSession2 = { 0x2c39ae40, 0x7d2e, 0x5044, { 0x80, 0x4e, 0x8b, 0x67, 0x99, 0xd4, 0xcf, 0x9e } };
const GUID kIID_IGraphicsCaptureSession3 = { 0xf2cdd966, 0x22ae, 0x5ea1, { 0x95, 0x96, 0x3a, 0x28, 0x93, 0x44, 0xc3, 0xbe } };
const GUID kIID_IDirect3DDxgiInterfaceAccess = { 0xa9b3d012, 0x3df2, 0x4ee3, { 0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1 } };

typedef HRESULT (WINAPI* PFN_RoInitialize)(int initType);
typedef HRESULT (WINAPI* PFN_RoGetActivationFactory)(HSTRING, REFIID, void**);
typedef HRESULT (WINAPI* PFN_WindowsCreateStringReference)(PCWSTR, UINT32, HSTRING_HEADER*, HSTRING*);
typedef HRESULT (WINAPI* PFN_CreateDirect3D11DeviceFromDXGIDevice)(IDXGIDevice*, IUnknown**);

struct WgcFunctions {
    PFN_RoInitialize roInitialize = nullptr;
    PFN_RoGetActivationFactory roGetActivationFactory = nullptr;
    PFN_WindowsCreateStringReference createStringRef = nullptr;
    PFN_CreateDirect3D11DeviceFromDXGIDevice createWgcDevice = nullptr;
    bool loaded = false;

    bool ensureLoaded() {
        if (loaded) return createWgcDevice != nullptr;
        loaded = true;
        HMODULE hCombase = LoadLibraryW(L"combase.dll");
        HMODULE hD3d11 = LoadLibraryW(L"d3d11.dll");
        if (!hCombase || !hD3d11) return false;
        roInitialize = reinterpret_cast<PFN_RoInitialize>(GetProcAddress(hCombase, "RoInitialize"));
        roGetActivationFactory = reinterpret_cast<PFN_RoGetActivationFactory>(GetProcAddress(hCombase, "RoGetActivationFactory"));
        createStringRef = reinterpret_cast<PFN_WindowsCreateStringReference>(GetProcAddress(hCombase, "WindowsCreateStringReference"));
        createWgcDevice = reinterpret_cast<PFN_CreateDirect3D11DeviceFromDXGIDevice>(
            GetProcAddress(hD3d11, "CreateDirect3D11DeviceFromDXGIDevice"));
        if (!roInitialize || !roGetActivationFactory || !createStringRef || !createWgcDevice) {
            createWgcDevice = nullptr;
            return false;
        }
        return true;
    }
};

WgcFunctions& wgcApi() {
    static WgcFunctions s_api;
    return s_api;
}

template <typename T>
void closeAndReleaseWinRt(void*& ptr) {
    if (!ptr) return;
    WgcClosable* closable = nullptr;
    WgcClosable* self = reinterpret_cast<WgcClosable*>(ptr);
    if (SUCCEEDED(self->lpVtbl->QueryInterface(self, kIID_IClosable, reinterpret_cast<void**>(&closable))) && closable) {
        closable->lpVtbl->Close(closable);
        closable->lpVtbl->Release(closable);
    }
    T* obj = reinterpret_cast<T*>(ptr);
    obj->lpVtbl->Release(obj);
    ptr = nullptr;
}

std::string adapterNameUtf8(const WCHAR* wideName) {
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wideName, -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 1) return "Default GPU";
    std::string out(utf8Len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wideName, -1, &out[0], utf8Len, nullptr, nullptr);
    return out;
}

} // namespace

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

    if (initGpuCapture()) {
        const char* tag = (m_mode == CaptureMode::Wgc) ? "WGC GPU Capture" : "DXGI GPU Capture";
        std::cout << "[ScreenCapture] " << tag << " (" << m_width << "x" << m_height
                  << ", " << getPixelFormatName() << ")" << std::endl;
        return true;
    }

    std::cout << "[ScreenCapture] GPU capture unavailable - falling back to GDI capture..." << std::endl;
    if (initGdi()) {
        std::cout << "[ScreenCapture] GDI Capture (" << m_width << "x" << m_height << ")" << std::endl;
        m_mode = CaptureMode::Gdi;
        m_lastGdiRetryMs = GetTickCount64();
        return true;
    }

    return false;
}

const char* ScreenCapture::getPixelFormatName() const {
    if (m_mode == CaptureMode::Gdi) return "GDI BGRA 8-bit";
    if (m_mode == CaptureMode::Wgc) {
        return isHdrOutput() ? "WGC 16-bit scRGB HDR" : "WGC 8-bit BGRA";
    }
    switch (m_format) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "16-bit scRGB HDR";
        case DXGI_FORMAT_R10G10B10A2_UNORM:  return "10-bit RGB10A2";
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "8-bit RGBA";
        default: return "8-bit BGRA";
    }
}

void ScreenCapture::cleanup() {
    cleanupGpu();
    cleanupGdi();
}

bool ScreenCapture::initGpuCapture() {
    // 1) Try live DXGI Desktop Duplication first (best HDR16 & dirty-rect latency)
    if (initDxgi(true)) {
        m_mode = CaptureMode::Dxgi;
        m_gpuReinitFails = 0;
        return true;
    }
    // 2) Try Windows.Graphics.Capture next on calm desktops so DWM gives real composited pixels
    if (initWgc()) {
        m_mode = CaptureMode::Wgc;
        m_gpuReinitFails = 0;
        return true;
    }
    // 3) Static DXGI Desktop Duplication seeded with real GDI snapshot when WGC is unavailable
    if (initDxgi(false)) {
        m_mode = CaptureMode::Dxgi;
        m_gpuReinitFails = 0;
        return true;
    }
    return false;
}

bool ScreenCapture::createD3d11DeviceForAdapter(IDXGIAdapter1* adapter) {
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL chosenLevel;

    auto tryCreate = [&](UINT flags) -> HRESULT {
        return D3D11CreateDevice(
            adapter,
            adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
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

    HRESULT hr = tryCreate(D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT);
    if (FAILED(hr)) {
        hr = tryCreate(D3D11_CREATE_DEVICE_BGRA_SUPPORT);
    }
    if (FAILED(hr) || !m_d3dDevice || !m_d3dContext) {
        return false;
    }

    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED(m_d3dDevice->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&mt)))) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }

    IDXGIDevice* dxgiDevice = nullptr;
    if (SUCCEEDED(m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) && dxgiDevice) {
        dxgiDevice->SetGPUThreadPriority(7);
        IDXGIAdapter* dxgiAdapter = nullptr;
        if (SUCCEEDED(dxgiDevice->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&dxgiAdapter))) && dxgiAdapter) {
            DXGI_ADAPTER_DESC desc = {};
            if (SUCCEEDED(dxgiAdapter->GetDesc(&desc))) {
                m_isNvidiaGpu = (desc.VendorId == 0x10DE);
                m_adapterName = adapterNameUtf8(desc.Description);
            }
            dxgiAdapter->Release();
        }
        dxgiDevice->Release();
    }
    return true;
}

bool ScreenCapture::ensureGpuCopyTextures(uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
    if (!m_d3dDevice || w == 0 || h == 0) return false;

    std::lock_guard<std::mutex> lock(m_snapMutex);
    if (m_gpuCopyTex[0] && m_gpuCopyTex[1] && m_width == w && m_height == h && m_format == fmt) {
        return true;
    }

    for (int i = 0; i < 2; ++i) {
        if (m_gpuCopyTex[i]) { m_gpuCopyTex[i]->Release(); m_gpuCopyTex[i] = nullptr; }
    }
    if (m_stagingTexture) { m_stagingTexture->Release(); m_stagingTexture = nullptr; }
    m_latestSlot.store(0);

    m_width = w;
    m_height = h;
    m_format = fmt;
    if (m_dxgiOutput6) {
        DXGI_OUTPUT_DESC1 desc1 = {};
        if (SUCCEEDED(m_dxgiOutput6->GetDesc1(&desc1))) {
            m_isHdrOutput = (desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        }
    }

    D3D11_TEXTURE2D_DESC gpuDesc = {};
    gpuDesc.Width = m_width;
    gpuDesc.Height = m_height;
    gpuDesc.MipLevels = 1;
    gpuDesc.ArraySize = 1;
    gpuDesc.Format = m_format;
    gpuDesc.SampleDesc.Count = 1;
    gpuDesc.Usage = D3D11_USAGE_DEFAULT;
    gpuDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    for (int i = 0; i < 2; ++i) {
        HRESULT hr = m_d3dDevice->CreateTexture2D(&gpuDesc, nullptr, &m_gpuCopyTex[i]);
        if (FAILED(hr) || !m_gpuCopyTex[i]) {
            for (int j = 0; j < 2; ++j) {
                if (m_gpuCopyTex[j]) { m_gpuCopyTex[j]->Release(); m_gpuCopyTex[j] = nullptr; }
            }
            return false;
        }
    }
    return true;
}

bool ScreenCapture::initDxgi(bool requireLiveOnly) {
    cleanupGpu();

    HMONITOR primaryMon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    bool hasPrimaryInfo = (GetMonitorInfoW(primaryMon, &mi) != FALSE);

    std::vector<IDXGIAdapter1*> nvPrimary, otherPrimary, nvSecondary, otherSecondary;
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory) {
        for (UINT idx = 0; ; ++idx) {
            IDXGIAdapter1* cand = nullptr;
            if (factory->EnumAdapters1(idx, &cand) == DXGI_ERROR_NOT_FOUND || !cand) break;

            DXGI_ADAPTER_DESC1 ad = {};
            cand->GetDesc1(&ad);
            if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                cand->Release();
                continue;
            }

            bool hasOutput = false;
            bool ownsPrimary = false;
            for (UINT oi = 0; ; ++oi) {
                IDXGIOutput* out = nullptr;
                if (cand->EnumOutputs(oi, &out) == DXGI_ERROR_NOT_FOUND || !out) break;
                DXGI_OUTPUT_DESC od = {};
                if (SUCCEEDED(out->GetDesc(&od)) && od.AttachedToDesktop &&
                    od.DesktopCoordinates.right > od.DesktopCoordinates.left &&
                    od.DesktopCoordinates.bottom > od.DesktopCoordinates.top) {
                    hasOutput = true;
                    if (hasPrimaryInfo && _wcsicmp(od.DeviceName, mi.szDevice) == 0) {
                        ownsPrimary = true;
                    }
                }
                out->Release();
                if (ownsPrimary) break;
            }

            if (hasOutput) {
                if (ad.VendorId == 0x10DE) {
                    (ownsPrimary ? nvPrimary : nvSecondary).push_back(cand);
                } else {
                    (ownsPrimary ? otherPrimary : otherSecondary).push_back(cand);
                }
                continue;
            }
            cand->Release();
        }
        factory->Release();
    }

    const RECT* primaryRect = hasPrimaryInfo ? &mi.rcMonitor : nullptr;
    bool ok = false;
    auto tryGroup = [&](const std::vector<IDXGIAdapter1*>& group) {
        for (IDXGIAdapter1* cand : group) {
            if (!ok) ok = initDxgiOnAdapter(cand, requireLiveOnly, primaryRect);
        }
    };
    // Prefer adapter actually owning the primary desktop (0,0) before secondary outputs
    tryGroup(nvPrimary);
    tryGroup(otherPrimary);
    tryGroup(nvSecondary);
    tryGroup(otherSecondary);

    for (auto* list : { &nvPrimary, &otherPrimary, &nvSecondary, &otherSecondary }) {
        for (IDXGIAdapter1* cand : *list) cand->Release();
    }
    if (!ok && !requireLiveOnly) {
        ok = initDxgiOnAdapter(nullptr, false, primaryRect);
    }
    return ok;
}

bool ScreenCapture::initDxgiOnAdapter(IDXGIAdapter1* adapter, bool requireLivePresent, const RECT* primaryMonitorRect) {
    cleanupGpu();

    if (!createD3d11DeviceForAdapter(adapter)) {
        cleanupGpu();
        return false;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    if (FAILED(m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) || !dxgiDevice) {
        cleanupGpu();
        return false;
    }
    IDXGIAdapter* dxgiAdapter = nullptr;
    HRESULT hr = dxgiDevice->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&dxgiAdapter));
    dxgiDevice->Release();
    if (FAILED(hr) || !dxgiAdapter) {
        cleanupGpu();
        return false;
    }

    struct OutCand { IDXGIOutput* out; bool isPrimary; };
    std::vector<OutCand> attachedOutputs;
    for (UINT oi = 0; ; ++oi) {
        IDXGIOutput* out = nullptr;
        if (dxgiAdapter->EnumOutputs(oi, &out) == DXGI_ERROR_NOT_FOUND || !out) break;
        DXGI_OUTPUT_DESC od = {};
        if (SUCCEEDED(out->GetDesc(&od)) && od.AttachedToDesktop &&
            od.DesktopCoordinates.right > od.DesktopCoordinates.left &&
            od.DesktopCoordinates.bottom > od.DesktopCoordinates.top) {
            bool isPrimary = primaryMonitorRect &&
                EqualRect(&od.DesktopCoordinates, primaryMonitorRect);
            attachedOutputs.push_back({ out, isPrimary });
        } else {
            out->Release();
        }
    }
    dxgiAdapter->Release();
    if (attachedOutputs.empty()) {
        cleanupGpu();
        return false;
    }

    std::stable_sort(attachedOutputs.begin(), attachedOutputs.end(),
                     [](const OutCand& a, const OutCand& b) { return a.isPrimary && !b.isPrimary; });

    bool duplicated = false;
    for (size_t oi = 0; oi < attachedOutputs.size(); ++oi) {
        IDXGIOutput* dxgiOutput = attachedOutputs[oi].out;
        if (duplicated) {
            dxgiOutput->Release();
            continue;
        }

        IDXGIOutput6* dxgiOutput6 = nullptr;
        if (SUCCEEDED(dxgiOutput->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&dxgiOutput6)))) {
            DXGI_OUTPUT_DESC1 desc1 = {};
            if (SUCCEEDED(dxgiOutput6->GetDesc1(&desc1))) {
                m_isHdrOutput = (desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
            }
            if (m_dxgiOutput6) m_dxgiOutput6->Release();
            m_dxgiOutput6 = dxgiOutput6;
        }

        IDXGIOutput5* dxgiOutput5 = nullptr;
        if (SUCCEEDED(dxgiOutput->QueryInterface(__uuidof(IDXGIOutput5), reinterpret_cast<void**>(&dxgiOutput5)))) {
            DXGI_OUTPUT_DESC outputDesc = {};
            dxgiOutput5->GetDesc(&outputDesc);
            m_screenLeft = outputDesc.DesktopCoordinates.left;
            m_screenTop = outputDesc.DesktopCoordinates.top;
            m_width = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
            m_height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;
            m_refreshRateHz = queryMonitorRefreshHz(outputDesc.DeviceName);

            const DXGI_FORMAT supportedFormats[] = {
                DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R10G10B10A2_UNORM,
                DXGI_FORMAT_B8G8R8A8_UNORM
            };
            hr = dxgiOutput5->DuplicateOutput1(m_d3dDevice, 0, ARRAYSIZE(supportedFormats), supportedFormats, &m_duplication);
            dxgiOutput5->Release();
        } else {
            IDXGIOutput1* dxgiOutput1 = nullptr;
            if (SUCCEEDED(dxgiOutput->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&dxgiOutput1)))) {
                DXGI_OUTPUT_DESC outputDesc = {};
                dxgiOutput1->GetDesc(&outputDesc);
                m_screenLeft = outputDesc.DesktopCoordinates.left;
                m_screenTop = outputDesc.DesktopCoordinates.top;
                m_width = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
                m_height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;
                m_refreshRateHz = queryMonitorRefreshHz(outputDesc.DeviceName);
                hr = dxgiOutput1->DuplicateOutput(m_d3dDevice, &m_duplication);
                dxgiOutput1->Release();
            } else {
                hr = E_FAIL;
            }
        }
        dxgiOutput->Release();

        if (SUCCEEDED(hr) && m_duplication) {
            const int maxProbes = requireLivePresent ? 6 : 3;
            for (int a = 0; a < maxProbes && !acquireDxgi(15, requireLivePresent); ++a) {}
            if (m_duplication && !m_hasValidGpuFrame && !requireLivePresent) {
                seedGpuTextureFromGdi();
            }
            if (m_duplication && m_hasValidGpuFrame) {
                duplicated = true;
            } else {
                if (m_duplication) { m_duplication->Release(); m_duplication = nullptr; }
                if (m_dxgiOutput6) { m_dxgiOutput6->Release(); m_dxgiOutput6 = nullptr; }
            }
        }
    }

    if (!duplicated) {
        cleanupGpu();
        return false;
    }

    std::cout << "[ScreenCapture] GPU Adapter: " << m_adapterName
              << (m_isNvidiaGpu ? " (NVIDIA Dedicated)" : " (Display Owner)") << std::endl;
    return true;
}

void ScreenCapture::cleanupWgcInterfaces() {
    closeAndReleaseWinRt<WgcCaptureSession>(m_wgcSession);
    closeAndReleaseWinRt<WgcFramePool>(m_wgcPool);
    if (m_wgcItem) {
        reinterpret_cast<WgcCaptureItem*>(m_wgcItem)->lpVtbl->Release(reinterpret_cast<WgcCaptureItem*>(m_wgcItem));
        m_wgcItem = nullptr;
    }
    if (m_wgcD3dDevice) {
        reinterpret_cast<IUnknown*>(m_wgcD3dDevice)->Release();
        m_wgcD3dDevice = nullptr;
    }
    m_wgcPoolW = 0;
    m_wgcPoolH = 0;
}

void ScreenCapture::cleanupGpu() {
    cleanupWgcInterfaces();
    std::lock_guard<std::mutex> lock(m_snapMutex);
    for (int i = 0; i < 2; ++i) {
        if (m_gpuCopyTex[i]) { m_gpuCopyTex[i]->Release(); m_gpuCopyTex[i] = nullptr; }
    }
    m_latestSlot.store(0);
    m_hasValidGpuFrame = false;
    if (m_stagingTexture) { m_stagingTexture->Release(); m_stagingTexture = nullptr; }
    if (m_duplication) { m_duplication->Release(); m_duplication = nullptr; }
    if (m_dxgiOutput6) { m_dxgiOutput6->Release(); m_dxgiOutput6 = nullptr; }
    if (m_d3dContext) { m_d3dContext->Release(); m_d3dContext = nullptr; }
    if (m_d3dDevice) { m_d3dDevice->Release(); m_d3dDevice = nullptr; }
    if (m_mode == CaptureMode::Dxgi || m_mode == CaptureMode::Wgc) {
        m_mode = CaptureMode::None;
    }
}

bool ScreenCapture::initWgc() {
    if (!wgcApi().ensureLoaded()) return false;

    HMONITOR hMon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) {
        mi.rcMonitor = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        mi.szDevice[0] = L'\0';
    }
    m_refreshRateHz = queryMonitorRefreshHz(mi.szDevice);

    IDXGIAdapter1* primaryAdapter = nullptr;
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory) {
        for (UINT idx = 0; !primaryAdapter; ++idx) {
            IDXGIAdapter1* cand = nullptr;
            if (factory->EnumAdapters1(idx, &cand) == DXGI_ERROR_NOT_FOUND || !cand) break;
            DXGI_ADAPTER_DESC1 ad = {};
            cand->GetDesc1(&ad);
            if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                for (UINT oi = 0; ; ++oi) {
                    IDXGIOutput* out = nullptr;
                    if (cand->EnumOutputs(oi, &out) == DXGI_ERROR_NOT_FOUND || !out) break;
                    DXGI_OUTPUT_DESC od = {};
                    bool match = SUCCEEDED(out->GetDesc(&od)) && od.AttachedToDesktop &&
                        (mi.szDevice[0] == L'\0' || _wcsicmp(od.DeviceName, mi.szDevice) == 0);
                    out->Release();
                    if (match) {
                        primaryAdapter = cand;
                        break;
                    }
                }
            }
            if (!primaryAdapter) cand->Release();
        }
        factory->Release();
    }

    bool ok = initWgcOnAdapter(primaryAdapter, hMon, mi.rcMonitor);
    if (!ok && primaryAdapter) {
        ok = initWgcOnAdapter(nullptr, hMon, mi.rcMonitor);
    }
    if (primaryAdapter) primaryAdapter->Release();
    return ok;
}

bool ScreenCapture::initWgcOnAdapter(IDXGIAdapter1* adapter, HMONITOR hMon, const RECT& monRect) {
    cleanupGpu();
    if (!wgcApi().ensureLoaded()) return false;

    HRESULT roHr = wgcApi().roInitialize(1 /* RO_INIT_MULTITHREADED */);
    if (FAILED(roHr) && roHr != RPC_E_CHANGED_MODE) {
        return false;
    }

    if (!createD3d11DeviceForAdapter(adapter)) {
        cleanupGpu();
        return false;
    }

    m_screenLeft = monRect.left;
    m_screenTop = monRect.top;
    m_width = static_cast<uint32_t>(std::max<LONG>(0, monRect.right - monRect.left));
    m_height = static_cast<uint32_t>(std::max<LONG>(0, monRect.bottom - monRect.top));
    m_isHdrOutput = false;

    IDXGIDevice* dxgiDevice = nullptr;
    if (FAILED(m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) || !dxgiDevice) {
        cleanupGpu();
        return false;
    }

    // Check IDXGIOutput6 HDR state on this adapter's primary output before configuring pool format
    IDXGIAdapter* dxgiAdp = nullptr;
    if (SUCCEEDED(dxgiDevice->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&dxgiAdp))) && dxgiAdp) {
        for (UINT oi = 0; ; ++oi) {
            IDXGIOutput* out = nullptr;
            if (dxgiAdp->EnumOutputs(oi, &out) == DXGI_ERROR_NOT_FOUND || !out) break;
            IDXGIOutput6* out6 = nullptr;
            if (SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&out6))) && out6) {
                DXGI_OUTPUT_DESC1 d1 = {};
                if (SUCCEEDED(out6->GetDesc1(&d1)) && d1.AttachedToDesktop) {
                    if (EqualRect(&d1.DesktopCoordinates, &monRect) || oi == 0) {
                        m_isHdrOutput = (d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
                        m_dxgiOutput6 = out6;
                        out->Release();
                        break;
                    }
                }
                out6->Release();
            }
            out->Release();
        }
        dxgiAdp->Release();
    }

    IUnknown* wgcDev = nullptr;
    HRESULT hr = wgcApi().createWgcDevice(dxgiDevice, &wgcDev);
    dxgiDevice->Release();
    if (FAILED(hr) || !wgcDev) {
        cleanupGpu();
        return false;
    }
    m_wgcD3dDevice = wgcDev;

    const wchar_t* itemClass = L"Windows.Graphics.Capture.GraphicsCaptureItem";
    HSTRING_HEADER itemHdr = {};
    HSTRING itemHs = nullptr;
    WgcCaptureItemInterop* interop = nullptr;
    if (FAILED(wgcApi().createStringRef(itemClass, static_cast<UINT32>(std::wcslen(itemClass)), &itemHdr, &itemHs)) ||
        FAILED(wgcApi().roGetActivationFactory(itemHs, kIID_IGraphicsCaptureItemInterop, reinterpret_cast<void**>(&interop))) ||
        !interop) {
        cleanupGpu();
        return false;
    }

    WgcCaptureItem* item = nullptr;
    hr = interop->lpVtbl->CreateForMonitor(interop, hMon, kIID_IGraphicsCaptureItem, reinterpret_cast<void**>(&item));
    interop->lpVtbl->Release(interop);
    if (FAILED(hr) || !item) {
        cleanupGpu();
        return false;
    }
    m_wgcItem = item;

    WgcSizeInt32 itemSize = { static_cast<int32_t>(m_width), static_cast<int32_t>(m_height) };
    WgcSizeInt32 reportedSize = {};
    if (SUCCEEDED(item->lpVtbl->get_Size(item, &reportedSize)) && reportedSize.Width > 0 && reportedSize.Height > 0) {
        itemSize = reportedSize;
    }
    if (itemSize.Width <= 0 || itemSize.Height <= 0) {
        cleanupGpu();
        return false;
    }

    const wchar_t* poolClass = L"Windows.Graphics.Capture.Direct3D11CaptureFramePool";
    HSTRING_HEADER poolHdr = {};
    HSTRING poolHs = nullptr;
    WgcFramePoolStatics* statics2 = nullptr;
    if (FAILED(wgcApi().createStringRef(poolClass, static_cast<UINT32>(std::wcslen(poolClass)), &poolHdr, &poolHs)) ||
        FAILED(wgcApi().roGetActivationFactory(poolHs, kIID_IDirect3D11CaptureFramePoolStatics2, reinterpret_cast<void**>(&statics2))) ||
        !statics2) {
        cleanupGpu();
        return false;
    }

    std::vector<DXGI_FORMAT> fmtCandidates;
    if (m_isHdrOutput) fmtCandidates.push_back(DXGI_FORMAT_R16G16B16A16_FLOAT);
    fmtCandidates.push_back(DXGI_FORMAT_B8G8R8A8_UNORM);

    bool started = false;
    for (DXGI_FORMAT candFmt : fmtCandidates) {
        closeAndReleaseWinRt<WgcCaptureSession>(m_wgcSession);
        closeAndReleaseWinRt<WgcFramePool>(m_wgcPool);

        WgcFramePool* pool = nullptr;
        hr = statics2->lpVtbl->Create(statics2, wgcDev, static_cast<int32_t>(candFmt), 3, itemSize, &pool);
        if (FAILED(hr) || !pool) continue;
        m_wgcPool = pool;
        m_wgcPoolW = itemSize.Width;
        m_wgcPoolH = itemSize.Height;
        m_wgcPoolFmt = candFmt;

        WgcCaptureSession* session = nullptr;
        if (FAILED(pool->lpVtbl->CreateCaptureSession(pool, item, &session)) || !session) continue;
        m_wgcSession = session;

        // Hide OS cursor from WGC pixels (we stream crisp hardware cursor out-of-band) and hide Win11 yellow border
        WgcCaptureSession2* s2 = nullptr;
        if (SUCCEEDED(session->lpVtbl->QueryInterface(session, kIID_IGraphicsCaptureSession2, reinterpret_cast<void**>(&s2))) && s2) {
            s2->lpVtbl->put_IsCursorCaptureEnabled(s2, 0);
            s2->lpVtbl->Release(s2);
        }
        WgcCaptureSession3* s3 = nullptr;
        if (SUCCEEDED(session->lpVtbl->QueryInterface(session, kIID_IGraphicsCaptureSession3, reinterpret_cast<void**>(&s3))) && s3) {
            s3->lpVtbl->put_IsBorderRequired(s3, 0);
            s3->lpVtbl->Release(s3);
        }

        if (FAILED(session->lpVtbl->StartCapture(session))) continue;

        for (int waitIdx = 0; waitIdx < 20 && !acquireWgc(); ++waitIdx) {
            Sleep(10);
        }
        if (m_hasValidGpuFrame) {
            if (candFmt != DXGI_FORMAT_R16G16B16A16_FLOAT) {
                m_isHdrOutput = false;
            }
            started = true;
            break;
        }
    }
    statics2->lpVtbl->Release(statics2);

    if (!started) {
        cleanupGpu();
        return false;
    }

    std::cout << "[ScreenCapture] WGC Adapter: " << m_adapterName
              << (m_isNvidiaGpu ? " (NVIDIA Dedicated)" : " (Display Owner)") << std::endl;
    return true;
}

bool ScreenCapture::acquireWgc() {
    if (!m_wgcPool || !m_d3dDevice || !m_d3dContext) return false;

    WgcFramePool* pool = reinterpret_cast<WgcFramePool*>(m_wgcPool);
    WgcCaptureFrame* latest = nullptr;
    for (;;) {
        WgcCaptureFrame* next = nullptr;
        HRESULT hr = pool->lpVtbl->TryGetNextFrame(pool, &next);
        if (FAILED(hr)) {
            if (latest) latest->lpVtbl->Release(latest);
            cleanupGpu();
            return false;
        }
        if (!next) break;
        if (latest) latest->lpVtbl->Release(latest);
        latest = next;
    }
    if (!latest) return false;

    WgcSizeInt32 contentSize = { m_wgcPoolW, m_wgcPoolH };
    latest->lpVtbl->get_ContentSize(latest, &contentSize);

    IUnknown* surfaceUnk = nullptr;
    HRESULT hr = latest->lpVtbl->get_Surface(latest, &surfaceUnk);
    if (FAILED(hr) || !surfaceUnk) {
        latest->lpVtbl->Release(latest);
        return false;
    }

    WgcDxgiInterfaceAccess* access = nullptr;
    hr = surfaceUnk->QueryInterface(kIID_IDirect3DDxgiInterfaceAccess, reinterpret_cast<void**>(&access));
    surfaceUnk->Release();
    if (FAILED(hr) || !access) {
        latest->lpVtbl->Release(latest);
        return false;
    }

    ID3D11Texture2D* srcTex = nullptr;
    hr = access->lpVtbl->GetInterface(access, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&srcTex));
    access->lpVtbl->Release(access);
    if (FAILED(hr) || !srcTex) {
        latest->lpVtbl->Release(latest);
        return false;
    }

    D3D11_TEXTURE2D_DESC actualDesc = {};
    srcTex->GetDesc(&actualDesc);
    uint32_t copyW = actualDesc.Width;
    uint32_t copyH = actualDesc.Height;
    if (contentSize.Width > 0 && static_cast<uint32_t>(contentSize.Width) < copyW) {
        copyW = static_cast<uint32_t>(contentSize.Width);
    }
    if (contentSize.Height > 0 && static_cast<uint32_t>(contentSize.Height) < copyH) {
        copyH = static_cast<uint32_t>(contentSize.Height);
    }

    bool copied = false;
    if (ensureGpuCopyTextures(copyW, copyH, actualDesc.Format)) {
        int writeSlot = 1 - m_latestSlot.load();
        ID3D11Texture2D* dstTex = m_gpuCopyTex[writeSlot];
        D3D11_BOX srcBox = { 0, 0, 0, copyW, copyH, 1 };
        m_d3dContext->CopySubresourceRegion(dstTex, 0, 0, 0, 0, srcTex, 0, &srcBox);
        m_d3dContext->Flush();

        {
            std::lock_guard<std::mutex> lock(m_snapMutex);
            m_latestSlot.store(writeSlot);
            m_hasValidGpuFrame = true;
            if (m_mode == CaptureMode::Wgc && m_wgcPoolFmt != DXGI_FORMAT_R16G16B16A16_FLOAT) {
                m_isHdrOutput = false;
            }
        }
        copied = true;
    }

    srcTex->Release();
    latest->lpVtbl->Release(latest);

    if (contentSize.Width > 0 && contentSize.Height > 0 &&
        (contentSize.Width != m_wgcPoolW || contentSize.Height != m_wgcPoolH)) {
        if (SUCCEEDED(pool->lpVtbl->Recreate(pool, reinterpret_cast<IUnknown*>(m_wgcD3dDevice),
                                             static_cast<int32_t>(m_wgcPoolFmt), 2, contentSize))) {
            m_wgcPoolW = contentSize.Width;
            m_wgcPoolH = contentSize.Height;
        }
    }
    return copied;
}

bool ScreenCapture::grabGpuFrame(ID3D11Device** outDev, ID3D11DeviceContext** outCtx, ID3D11Texture2D** outTex,
                                 uint32_t& outW, uint32_t& outH, DXGI_FORMAT& outFmt, bool& outHdr) {
    if (!outDev || !outCtx || !outTex) return false;
    *outDev = nullptr;
    *outCtx = nullptr;
    *outTex = nullptr;

    std::lock_guard<std::mutex> lock(m_snapMutex);
    int slot = m_latestSlot.load();
    ID3D11Texture2D* tex = m_gpuCopyTex[slot];
    if (!isDxgiActive() || !m_hasValidGpuFrame || !m_d3dDevice || !m_d3dContext || !tex) {
        return false;
    }

    m_d3dDevice->AddRef();
    m_d3dContext->AddRef();
    tex->AddRef();
    *outDev = m_d3dDevice;
    *outCtx = m_d3dContext;
    *outTex = tex;
    outW = m_width;
    outH = m_height;
    outFmt = m_format;
    outHdr = isHdrOutput();
    return true;
}

bool ScreenCapture::acquireFrame(uint32_t timeoutMs) {
    uint64_t nowMs = GetTickCount64();

    if (m_mode == CaptureMode::Dxgi && m_duplication) {
        if (acquireDxgi(timeoutMs, false)) {
            m_gpuReinitFails = 0;
            return true;
        }
        if (m_duplication) return false; // normal WAIT_TIMEOUT or cursor-only skip
    } else if (m_mode == CaptureMode::Wgc && m_wgcPool) {
        if (acquireWgc()) {
            m_gpuReinitFails = 0;
            return true;
        }
        if (m_wgcPool) {
            if (timeoutMs > 0) Sleep(std::min<uint32_t>(timeoutMs, 4));
            return false;
        }
    }

    if (m_mode == CaptureMode::Gdi || m_mode == CaptureMode::None) {
        if (cooldownElapsedMs(nowMs, m_lastGdiRetryMs, 2000)) {
            m_lastGdiRetryMs = nowMs;
            const uint32_t savedW = m_width;
            const uint32_t savedH = m_height;
            const int32_t savedLeft = m_screenLeft;
            const int32_t savedTop = m_screenTop;
            const DXGI_FORMAT savedFmt = m_format;
            const bool savedHdr = m_isHdrOutput;
            if (initGpuCapture()) {
                cleanupGdi();
                std::cout << "[ScreenCapture] Recovered back to GPU capture ("
                          << (m_mode == CaptureMode::Wgc ? "WGC" : "DXGI") << ")." << std::endl;
                return true;
            }
            if (m_mode == CaptureMode::Gdi) {
                m_width = savedW;
                m_height = savedH;
                m_screenLeft = savedLeft;
                m_screenTop = savedTop;
                m_format = savedFmt;
                m_isHdrOutput = savedHdr;
            }
        }
        if (m_mode == CaptureMode::Gdi) {
            uint64_t gdiMinMs = std::max<uint64_t>(4ULL, 1000ULL / std::max<uint32_t>(30u, m_refreshRateHz));
            if (!cooldownElapsedMs(nowMs, m_lastGdiCaptureMs, gdiMinMs)) {
                if (timeoutMs > 0) Sleep(std::min<uint32_t>(timeoutMs, 4));
                return false;
            }
            m_lastGdiCaptureMs = nowMs;
            return acquireGdi();
        }
    }

    // DXGI or WGC session lost (e.g. game fullscreen toggle / UAC / HDR toggle): throttled GPU re-init before GDI step-down
    if (cooldownElapsedMs(nowMs, m_lastGpuReinitMs, 200)) {
        m_lastGpuReinitMs = nowMs;
        if (initGpuCapture()) {
            cleanupGdi();
            std::cout << "[ScreenCapture] GPU capture re-acquired ("
                      << (m_mode == CaptureMode::Wgc ? "WGC" : "DXGI") << ")." << std::endl;
            return true;
        }
        if (shouldStepDownToGdi(++m_gpuReinitFails) && initGdi()) {
            std::cout << "[ScreenCapture] GPU capture temporarily lost - switched to GDI fallback (will auto-recover)." << std::endl;
            m_mode = CaptureMode::Gdi;
            m_lastGdiRetryMs = nowMs;
            return acquireGdi();
        }
    } else if (timeoutMs > 0) {
        Sleep(std::min<uint32_t>(timeoutMs, 4));
    }
    return false;
}

bool ScreenCapture::acquireDxgi(uint32_t timeoutMs, bool requireLivePresent) {
    if (!m_duplication || !m_d3dContext) return false;

    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
    IDXGIResource* desktopResource = nullptr;

    HRESULT hr = m_duplication->AcquireNextFrame(timeoutMs, &frameInfo, &desktopResource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;
    }
    if (FAILED(hr)) {
        cleanupGpu();
        return false;
    }

    if (!shouldAcceptDxgiFrame(static_cast<uint64_t>(frameInfo.LastPresentTime.QuadPart),
                               frameInfo.AccumulatedFrames, m_hasValidGpuFrame, requireLivePresent)) {
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

    if (!ensureGpuCopyTextures(actualDesc.Width, actualDesc.Height, actualDesc.Format)) {
        desktopImage->Release();
        m_duplication->ReleaseFrame();
        return false;
    }

    int writeSlot = 1 - m_latestSlot.load();
    ID3D11Texture2D* dstTex = m_gpuCopyTex[writeSlot];
    m_d3dContext->CopyResource(dstTex, desktopImage);
    m_d3dContext->Flush();
    desktopImage->Release();
    m_duplication->ReleaseFrame();

    {
        std::lock_guard<std::mutex> lock(m_snapMutex);
        m_latestSlot.store(writeSlot);
        m_hasValidGpuFrame = true;
    }
    return true;
}

bool ScreenCapture::copyCurrentFrameToBgra(RawFrame& outFrame) {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11Texture2D* copyTex = nullptr;
    ID3D11Texture2D* stagingTex = nullptr;
    uint32_t frameW = 0;
    uint32_t frameH = 0;
    DXGI_FORMAT frameFmt = DXGI_FORMAT_B8G8R8A8_UNORM;

    {
        std::lock_guard<std::mutex> lock(m_snapMutex);
        if (m_mode == CaptureMode::Gdi) {
            if (!m_gdiBits || m_width == 0 || m_height == 0) return false;
            outFrame.width = m_width;
            outFrame.height = m_height;
            outFrame.pitch = m_width * 4;
            size_t totalBytes = static_cast<size_t>(m_width) * m_height * 4;
            if (outFrame.bgra.size() != totalBytes) outFrame.bgra.resize(totalBytes);
            memcpy(outFrame.bgra.data(), m_gdiBits, totalBytes);
            return true;
        }

        if (!m_hasValidGpuFrame || !m_d3dDevice || !m_d3dContext || m_width == 0 || m_height == 0) {
            return false;
        }
        copyTex = m_gpuCopyTex[m_latestSlot.load()];
        if (!copyTex) return false;

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
            if (FAILED(m_d3dDevice->CreateTexture2D(&desc, nullptr, &m_stagingTexture)) || !m_stagingTexture) {
                return false;
            }
        }

        dev = m_d3dDevice;
        ctx = m_d3dContext;
        stagingTex = m_stagingTexture;
        dev->AddRef();
        ctx->AddRef();
        copyTex->AddRef();
        stagingTex->AddRef();
        frameW = m_width;
        frameH = m_height;
        frameFmt = m_format;
    }

    ctx->CopyResource(stagingTex, copyTex);
    copyTex->Release();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx->Map(stagingTex, 0, D3D11_MAP_READ, 0, &mapped))) {
        stagingTex->Release();
        ctx->Release();
        dev->Release();
        return false;
    }

    outFrame.width = frameW;
    outFrame.height = frameH;
    outFrame.pitch = frameW * 4;

    size_t totalBytes = static_cast<size_t>(frameW) * frameH * 4;
    if (outFrame.bgra.size() != totalBytes) {
        outFrame.bgra.resize(totalBytes);
    }

    const uint8_t* srcRow = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* dstRow = outFrame.bgra.data();

    if (frameFmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        for (uint32_t y = 0; y < frameH; ++y) {
            const HalfRGBA* srcPixel = reinterpret_cast<const HalfRGBA*>(srcRow);
            uint8_t* dstPixel = dstRow;
            for (uint32_t x = 0; x < frameW; ++x) {
                dstPixel[0] = s_halfToByteTable[srcPixel[x].b];
                dstPixel[1] = s_halfToByteTable[srcPixel[x].g];
                dstPixel[2] = s_halfToByteTable[srcPixel[x].r];
                dstPixel[3] = 255;
                dstPixel += 4;
            }
            srcRow += mapped.RowPitch;
            dstRow += (frameW * 4);
        }
    } else if (frameFmt == DXGI_FORMAT_R10G10B10A2_UNORM) {
        for (uint32_t y = 0; y < frameH; ++y) {
            const uint32_t* srcPixel = reinterpret_cast<const uint32_t*>(srcRow);
            uint8_t* dstPixel = dstRow;
            for (uint32_t x = 0; x < frameW; ++x) {
                uint32_t p = srcPixel[x];
                dstPixel[0] = static_cast<uint8_t>(((p >> 20) & 0x3FF) >> 2); // B
                dstPixel[1] = static_cast<uint8_t>(((p >> 10) & 0x3FF) >> 2); // G
                dstPixel[2] = static_cast<uint8_t>((p & 0x3FF) >> 2);         // R
                dstPixel[3] = 255;
                dstPixel += 4;
            }
            srcRow += mapped.RowPitch;
            dstRow += (frameW * 4);
        }
    } else if (frameFmt == DXGI_FORMAT_R8G8B8A8_UNORM || frameFmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        for (uint32_t y = 0; y < frameH; ++y) {
            const uint8_t* sp = srcRow;
            uint8_t* dp = dstRow;
            for (uint32_t x = 0; x < frameW; ++x) {
                dp[0] = sp[2]; // B
                dp[1] = sp[1]; // G
                dp[2] = sp[0]; // R
                dp[3] = 255;
                sp += 4;
                dp += 4;
            }
            srcRow += mapped.RowPitch;
            dstRow += (frameW * 4);
        }
    } else {
        size_t rowBytes = static_cast<size_t>(frameW) * 4;
        for (uint32_t y = 0; y < frameH; ++y) {
            memcpy(dstRow, srcRow, rowBytes);
            srcRow += mapped.RowPitch;
            dstRow += rowBytes;
        }
    }

    ctx->Unmap(stagingTex, 0);
    stagingTex->Release();
    ctx->Release();
    dev->Release();
    return true;
}

bool ScreenCapture::pollCursor(CursorPayload& outMeta, std::vector<uint8_t>& outBgra, bool forceBitmap) {
    CURSORINFO ci = { sizeof(CURSORINFO) };
    if (!GetCursorInfo(&ci)) return false;

    bool visible = (ci.flags & CURSOR_SHOWING) && (ci.hCursor != nullptr);
    if (!visible && !m_lastCursorVisible && !forceBitmap) {
        // Ignore rapid hidden mouse warps inside fullscreen FPS games (e.g. Valorant)
        m_lastCursorPos = ci.ptScreenPos;
        return false;
    }

    int relX = ci.ptScreenPos.x - m_screenLeft;
    int relY = ci.ptScreenPos.y - m_screenTop;

    bool needBitmap = m_cachedCursorBgra.empty();
    bool shapeChanged = (ci.hCursor != m_lastHCursor) || (visible && needBitmap);
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

bool ScreenCapture::seedGpuTextureFromGdi() {
    if (!m_d3dDevice || !m_d3dContext || m_isHdrOutput || m_width == 0 || m_height == 0) {
        return false;
    }
    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) return false;
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        ReleaseDC(NULL, hdcScreen);
        return false;
    }
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(m_width);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(m_height);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    bool seeded = false;
    if (bmp && bits) {
        HGDIOBJ oldBmp = SelectObject(hdcMem, bmp);
        if (BitBlt(hdcMem, 0, 0, static_cast<int>(m_width), static_cast<int>(m_height),
                   hdcScreen, m_screenLeft, m_screenTop, SRCCOPY) &&
            ensureGpuCopyTextures(m_width, m_height, DXGI_FORMAT_B8G8R8A8_UNORM)) {
            int slot = 1 - m_latestSlot.load();
            m_d3dContext->UpdateSubresource(m_gpuCopyTex[slot], 0, nullptr, bits, m_width * 4, 0);
            m_d3dContext->Flush();
            std::lock_guard<std::mutex> lock(m_snapMutex);
            m_latestSlot.store(slot);
            m_hasValidGpuFrame = true;
            seeded = true;
        }
        SelectObject(hdcMem, oldBmp);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    return seeded;
}

bool ScreenCapture::initGdi() {
    std::lock_guard<std::mutex> lock(m_snapMutex);
    cleanupGdiLocked();

    m_width = GetSystemMetrics(SM_CXSCREEN);
    m_height = GetSystemMetrics(SM_CYSCREEN);
    m_refreshRateHz = queryMonitorRefreshHz(nullptr);
    m_screenLeft = 0;
    m_screenTop = 0;

    m_hdcScreen = GetDC(NULL);
    if (!m_hdcScreen) return false;

    m_hdcMem = CreateCompatibleDC(m_hdcScreen);
    if (!m_hdcMem) { cleanupGdiLocked(); return false; }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = m_width;
    bmi.bmiHeader.biHeight = -static_cast<LONG>(m_height);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    m_hBitmap = CreateDIBSection(m_hdcScreen, &bmi, DIB_RGB_COLORS, &m_gdiBits, NULL, 0);
    if (!m_hBitmap || !m_gdiBits) { cleanupGdiLocked(); return false; }

    SelectObject(m_hdcMem, m_hBitmap);
    BitBlt(m_hdcMem, 0, 0, m_width, m_height, m_hdcScreen, 0, 0, SRCCOPY);
    return true;
}

void ScreenCapture::cleanupGdiLocked() {
    if (m_hBitmap) { DeleteObject(m_hBitmap); m_hBitmap = nullptr; }
    if (m_hdcMem) { DeleteDC(m_hdcMem); m_hdcMem = nullptr; }
    if (m_hdcScreen) { ReleaseDC(NULL, m_hdcScreen); m_hdcScreen = nullptr; }
    m_gdiBits = nullptr;
}

void ScreenCapture::cleanupGdi() {
    std::lock_guard<std::mutex> lock(m_snapMutex);
    cleanupGdiLocked();
}

bool ScreenCapture::acquireGdi() {
    std::lock_guard<std::mutex> lock(m_snapMutex);
    if (!m_hdcMem || !m_gdiBits) return false;
    return BitBlt(m_hdcMem, 0, 0, m_width, m_height, m_hdcScreen, 0, 0, SRCCOPY) == TRUE;
}
