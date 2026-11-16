#include "GuiWindow.h"
#include "GlassCanvas.h"
#include "AppIconData.h"
#include "SecProtect.h"
#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>
#include <wincodec.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <thread>

struct DWriteTextEngine {
    static constexpr int kRoleCount = 5; // 0=Title, 1=Row, 2=Badge, 3=Body, 4=Small

    HRESULT coHr = E_FAIL;
    HMODULE hD2D = nullptr;
    HMODULE hDW = nullptr;
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dw = nullptr;
    IWICImagingFactory* wic = nullptr;
    IWICBitmap* wicBmp = nullptr;
    ID2D1RenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* whiteBrush = nullptr;
    IDWriteInlineObject* ellipsis = nullptr;
    IDWriteTextFormat* formats[kRoleCount] = {};
    int targetW = 0;
    int targetH = 0;
    bool ready = false;
    std::wstring displayFamily = L"Segoe UI";
    std::wstring bodyFamily = L"Segoe UI";

    DWriteTextEngine() = default;
    ~DWriteTextEngine() { cleanup(); }

    void releaseFormats() {
        for (int i = 0; i < kRoleCount; ++i) {
            if (formats[i]) { formats[i]->Release(); formats[i] = nullptr; }
        }
        if (ellipsis) { ellipsis->Release(); ellipsis = nullptr; }
    }

    void dropRenderTarget() {
        if (whiteBrush) { whiteBrush->Release(); whiteBrush = nullptr; }
        if (rt) { rt->Release(); rt = nullptr; }
        if (wicBmp) { wicBmp->Release(); wicBmp = nullptr; }
        targetW = 0;
        targetH = 0;
    }

    void cleanup() {
        releaseFormats();
        dropRenderTarget();
        if (wic) { wic->Release(); wic = nullptr; }
        if (dw) { dw->Release(); dw = nullptr; }
        if (d2d) { d2d->Release(); d2d = nullptr; }
        if (hDW) { FreeLibrary(hDW); hDW = nullptr; }
        if (hD2D) { FreeLibrary(hD2D); hD2D = nullptr; }
        if (SUCCEEDED(coHr)) { CoUninitialize(); coHr = E_FAIL; }
        ready = false;
    }

    static std::wstring pickFontFamily(IDWriteFontCollection* coll, const wchar_t* const* candidates, size_t count) {
        if (!coll) return L"Segoe UI";
        for (size_t i = 0; i < count; ++i) {
            UINT32 idx = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(coll->FindFamilyName(candidates[i], &idx, &exists)) && exists) {
                return candidates[i];
            }
        }
        return L"Segoe UI";
    }

    bool init() {
        if (ready) return true;
        if (FAILED(coHr)) {
            coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        }

        static constexpr GUID kClsidWicFactory = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
        static constexpr GUID kIidWicFactory = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };
        static constexpr GUID kIidD2D1Factory = { 0x06152247, 0x6f50, 0x465a, { 0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07 } };
        static constexpr GUID kIidDWriteFactory = { 0xb859ee5a, 0xd838, 0x4b5b, { 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48 } };

        if (!hD2D) {
            std::wstring sD2d = WM_OBF_W(L"d2d1.dll");
            hD2D = LoadLibraryW(sD2d.c_str());
            wm_sec::wipeWString(sD2d);
        }
        if (!hDW) {
            std::wstring sDw = WM_OBF_W(L"dwrite.dll");
            hDW = LoadLibraryW(sDw.c_str());
            wm_sec::wipeWString(sDw);
        }
        if (!hD2D || !hDW) return false;

        if (!d2d) {
            using PfnD2DCreate = HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);
            std::string fnName = WM_OBF("D2D1CreateFactory");
            auto pD2D = reinterpret_cast<PfnD2DCreate>(GetProcAddress(hD2D, fnName.c_str()));
            wm_sec::wipeString(fnName);
            if (!pD2D || FAILED(pD2D(D2D1_FACTORY_TYPE_SINGLE_THREADED, kIidD2D1Factory, nullptr, reinterpret_cast<void**>(&d2d)))) {
                return false;
            }
        }
        if (!dw) {
            using PfnDwCreate = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
            std::string fnName = WM_OBF("DWriteCreateFactory");
            auto pDW = reinterpret_cast<PfnDwCreate>(GetProcAddress(hDW, fnName.c_str()));
            wm_sec::wipeString(fnName);
            if (!pDW || FAILED(pDW(DWRITE_FACTORY_TYPE_SHARED, kIidDWriteFactory, reinterpret_cast<IUnknown**>(&dw)))) {
                return false;
            }
        }
        if (!wic) {
            if (FAILED(CoCreateInstance(kClsidWicFactory, nullptr, CLSCTX_INPROC_SERVER, kIidWicFactory, reinterpret_cast<void**>(&wic))) || !wic) {
                return false;
            }
        }

        IDWriteFontCollection* coll = nullptr;
        if (SUCCEEDED(dw->GetSystemFontCollection(&coll, FALSE)) && coll) {
            const wchar_t* dispCands[] = { L"Google Sans", L"Inter", L"Segoe UI Variable Display", L"Segoe UI Variable Text", L"Roboto", L"Segoe UI" };
            const wchar_t* bodyCands[] = { L"Google Sans", L"Inter", L"Segoe UI Variable Text", L"Roboto", L"Segoe UI" };
            displayFamily = pickFontFamily(coll, dispCands, ARRAYSIZE(dispCands));
            bodyFamily = pickFontFamily(coll, bodyCands, ARRAYSIZE(bodyCands));
            coll->Release();
        }
        ready = true;
        return true;
    }

    bool rebuildForDpi(UINT dpi) {
        if (!init()) return false;
        releaseFormats();
        const float scale = static_cast<float>(dpi) / 96.0f;
        struct Spec { const wchar_t* fam; DWRITE_FONT_WEIGHT wt; float px; };
        const Spec specs[kRoleCount] = {
            { displayFamily.c_str(), DWRITE_FONT_WEIGHT_SEMI_BOLD, 15.5f * scale }, // Title
            { bodyFamily.c_str(),    DWRITE_FONT_WEIGHT_MEDIUM,    13.5f * scale }, // Row
            { bodyFamily.c_str(),    DWRITE_FONT_WEIGHT_MEDIUM,    11.0f * scale }, // Badge
            { bodyFamily.c_str(),    DWRITE_FONT_WEIGHT_MEDIUM,    12.5f * scale }, // Body
            { bodyFamily.c_str(),    DWRITE_FONT_WEIGHT_NORMAL,    11.5f * scale }  // Small
        };
        for (int i = 0; i < kRoleCount; ++i) {
            if (FAILED(dw->CreateTextFormat(specs[i].fam, nullptr, specs[i].wt,
                                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                            specs[i].px, L"en-US", &formats[i]))) {
                return false;
            }
            formats[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        dw->CreateEllipsisTrimmingSign(formats[1], &ellipsis);
        return true;
    }

    bool ensureTargetSize(int w, int h) {
        if (rt && whiteBrush && wicBmp && w <= targetW && h <= targetH) return true;
        dropRenderTarget();
        int allocW = std::max(w, 512);
        int allocH = std::max(h, 96);
        static constexpr GUID kPixFmt32Pbgra = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10 } };
        if (FAILED(wic->CreateBitmap(static_cast<UINT>(allocW), static_cast<UINT>(allocH),
                                     kPixFmt32Pbgra, WICBitmapCacheOnLoad, &wicBmp)) || !wicBmp) {
            return false;
        }
        D2D1_RENDER_TARGET_PROPERTIES props = {};
        props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
        props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        props.dpiX = 96.0f;
        props.dpiY = 96.0f;
        if (FAILED(d2d->CreateWicBitmapRenderTarget(wicBmp, &props, &rt)) || !rt) {
            dropRenderTarget();
            return false;
        }
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        D2D1_COLOR_F whiteCol = { 1.0f, 1.0f, 1.0f, 1.0f };
        if (FAILED(rt->CreateSolidColorBrush(&whiteCol, nullptr, &whiteBrush)) || !whiteBrush) {
            dropRenderTarget();
            return false;
        }
        targetW = allocW;
        targetH = allocH;
        return true;
    }

    bool renderText(const std::wstring& txt, int roleIdx, UINT fmt, int boxW, int boxH,
                    uint32_t* outPbgra, int outStridePixels) {
        if (!ready || roleIdx < 0 || roleIdx >= kRoleCount || !formats[roleIdx] || !outPbgra) return false;
        if (!ensureTargetSize(boxW, boxH)) return false;

        IDWriteTextLayout* layout = nullptr;
        if (FAILED(dw->CreateTextLayout(txt.c_str(), static_cast<UINT32>(txt.size()), formats[roleIdx],
                                        static_cast<float>(boxW), static_cast<float>(boxH), &layout)) || !layout) {
            return false;
        }
        layout->SetTextAlignment((fmt & DT_CENTER) ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                   : ((fmt & DT_RIGHT) ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING));
        layout->SetParagraphAlignment((fmt & DT_VCENTER) ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if ((fmt & DT_END_ELLIPSIS) && ellipsis) {
            DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            layout->SetTrimming(&trim, ellipsis);
        }

        rt->BeginDraw();
        D2D1_COLOR_F zeroCol = { 0.0f, 0.0f, 0.0f, 0.0f };
        rt->Clear(&zeroCol);
        rt->DrawTextLayout(D2D1_POINT_2F{0.0f, 0.0f}, layout, whiteBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        HRESULT hrEnd = rt->EndDraw();
        layout->Release();
        if (FAILED(hrEnd)) {
            dropRenderTarget();
            return false;
        }

        WICRect wr = { 0, 0, boxW, boxH };
        UINT strideBytes = static_cast<UINT>(outStridePixels * 4);
        UINT bufBytes = static_cast<UINT>(boxH) * strideBytes;
        return SUCCEEDED(wicBmp->CopyPixels(&wr, strideBytes, bufBytes, reinterpret_cast<BYTE*>(outPbgra)));
    }
};

namespace {

constexpr UINT WM_APP_REFRESH = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;
constexpr UINT WM_APP_SPLASH_STATUS = WM_APP + 3;
constexpr UINT WM_APP_SPLASH_DONE = WM_APP + 4;
constexpr UINT_PTR TIMER_CARET_BLINK = 102;
constexpr UINT_PTR TIMER_SPLASH_ANIM = 103;

constexpr int ID_IP_BOX       = 1000;
constexpr int ID_BTN_CONNECT  = 1001;
constexpr int ID_BTN_SCAN     = 1002;
constexpr int ID_BTN_MINIMIZE = 1003;
constexpr int ID_BTN_CLOSE    = 1004;

constexpr int ID_TRAY_OPEN    = 2001;
constexpr int ID_TRAY_UPDATE  = 2003;
constexpr int ID_TRAY_QUIT    = 2005;

constexpr int BASE_W = 412;
constexpr int BASE_H = 330;
constexpr int SPLASH_W = 328;
constexpr int SPLASH_H = 92;
constexpr size_t MAX_VISIBLE_ROWS = 2;

int sc(int px, UINT dpi) {
    return MulDiv(px, static_cast<int>(dpi), 96);
}

float scf(float px, UINT dpi) {
    return px * (static_cast<float>(dpi) / 96.0f);
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], len);
    return w;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], len, nullptr, nullptr);
    return s;
}

UINT queryWindowDpi(HWND hwnd) {
    std::wstring u32 = WM_OBF_W(L"user32.dll");
    std::string fnName = WM_OBF("GetDpiForWindow");
    using PfnGetDpiForWindow = UINT(WINAPI*)(HWND);
    auto fn = wm_sec::resolveHiddenApi<PfnGetDpiForWindow>(u32, fnName);
    wm_sec::wipeWString(u32);
    wm_sec::wipeString(fnName);
    if (fn && hwnd) {
        UINT d = fn(hwnd);
        if (d >= 72) return d;
    }
    return 96;
}

bool enableGlassBackdrop(HWND hwnd) {
    bool active = false;
    bool acrylicSet = false;

    std::wstring dwmMod = WM_OBF_W(L"dwmapi.dll");
    HMODULE dwm = LoadLibraryW(dwmMod.c_str());
    wm_sec::wipeWString(dwmMod);

    using PfnSetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    PfnSetAttr pSetAttr = nullptr;

    if (dwm) {
        struct DwmMargins { int left, right, top, bottom; };
        using PfnIsComp = HRESULT(WINAPI*)(BOOL*);
        using PfnExtend = HRESULT(WINAPI*)(HWND, const DwmMargins*);

        std::string sIsComp = WM_OBF("DwmIsCompositionEnabled");
        std::string sExtend = WM_OBF("DwmExtendFrameIntoClientArea");
        std::string sSetAttr = WM_OBF("DwmSetWindowAttribute");

        auto pIsComp = reinterpret_cast<PfnIsComp>(GetProcAddress(dwm, sIsComp.c_str()));
        auto pExtend = reinterpret_cast<PfnExtend>(GetProcAddress(dwm, sExtend.c_str()));
        pSetAttr = reinterpret_cast<PfnSetAttr>(GetProcAddress(dwm, sSetAttr.c_str()));
        wm_sec::wipeString(sIsComp);
        wm_sec::wipeString(sExtend);
        wm_sec::wipeString(sSetAttr);

        BOOL comp = FALSE;
        if (pIsComp && SUCCEEDED(pIsComp(&comp)) && comp) {
            active = true;
            if (pExtend) {
                DwmMargins allGlass = { -1, -1, -1, -1 };
                pExtend(hwnd, &allGlass);
            }
            if (pSetAttr) {
                BOOL dark = TRUE;
                if (FAILED(pSetAttr(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark)))) {
                    pSetAttr(hwnd, 19, &dark, sizeof(dark));
                }
                DWORD cornerPref = 2; // DWMWCP_ROUND
                pSetAttr(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));
            }
        }
    }

    if (active) {
        // Apply true DWM Acrylic blur with subtle 7% base tint so client alpha=65 yields ~70% total transparency
        std::wstring u32 = WM_OBF_W(L"user32.dll");
        std::string sCompAttr = WM_OBF("SetWindowCompositionAttribute");
        struct AccentPolicy { int state, flags; DWORD gradColor; int animId; };
        struct WinCompAttr { DWORD attr; PVOID data; ULONG sizeOfData; };
        using PfnSetComp = BOOL(WINAPI*)(HWND, WinCompAttr*);
        auto pSetComp = wm_sec::resolveHiddenApi<PfnSetComp>(u32, sCompAttr);
        wm_sec::wipeWString(u32);
        wm_sec::wipeString(sCompAttr);
        if (pSetComp) {
            AccentPolicy acc = { 4 /* ACCENT_ENABLE_ACRYLICBLURBEHIND */, 2, 0x1208080Cu, 0 };
            WinCompAttr data = { 19 /* WCA_ACCENT_POLICY */, &acc, sizeof(acc) };
            if (pSetComp(hwnd, &data)) {
                acrylicSet = true;
            } else {
                acc.state = 3; // ACCENT_ENABLE_BLURBEHIND
                acc.gradColor = 0;
                acrylicSet = (pSetComp(hwnd, &data) == TRUE);
            }
        }
        if (!acrylicSet && pSetAttr) {
            DWORD backdrop = 3; // DWMSBT_TRANSIENTWINDOW fallback if SetWindowCompositionAttribute fails
            pSetAttr(hwnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &backdrop, sizeof(backdrop));
        }
    }

    if (dwm) FreeLibrary(dwm);
    return active;
}

struct HitRectItem {
    int id;
    RECT rc;
};

RECT receiverRowRect(int idx, UINT dpi) {
    return {
        sc(24, dpi),
        sc(69 + idx * 70, dpi),
        sc(388, dpi),
        sc(131 + idx * 70, dpi)
    };
}

std::vector<HitRectItem> buildControlRects(UINT dpi) {
    return {
        { ID_BTN_MINIMIZE, { sc(344, dpi), sc(16,  dpi), sc(368, dpi), sc(40,  dpi) } },
        { ID_BTN_CLOSE,    { sc(374, dpi), sc(16,  dpi), sc(398, dpi), sc(40,  dpi) } },
        { ID_IP_BOX,       { sc(16,  dpi), sc(222, dpi), sc(222, dpi), sc(258, dpi) } },
        { ID_BTN_CONNECT,  { sc(230, dpi), sc(222, dpi), sc(320, dpi), sc(258, dpi) } },
        { ID_BTN_SCAN,     { sc(328, dpi), sc(222, dpi), sc(396, dpi), sc(258, dpi) } }
    };
}

void drawLaptopGlyph(uint32_t* px, int w, int h, float x, float y, UINT dpi, GlassColor col) {
    float s = scf(1.0f, dpi);
    glass::strokeRoundRect(px, w, h, x + 3 * s, y + 1 * s, x + 21 * s, y + 13 * s, 2.2f * s, 1.6f * s, col);
    glass::drawAaSegment(px, w, h, x, y + 16 * s, x + 24 * s, y + 16 * s, 1.7f * s, col);
}

void drawTvGlyph(uint32_t* px, int w, int h, float x, float y, UINT dpi, GlassColor col) {
    float s = scf(1.0f, dpi);
    glass::strokeRoundRect(px, w, h, x + 2 * s, y, x + 22 * s, y + 14 * s, 2.2f * s, 1.6f * s, col);
    glass::drawAaSegment(px, w, h, x + 7 * s, y + 17 * s, x + 17 * s, y + 17 * s, 1.7f * s, col);
}

void drawMonitorGlyph(uint32_t* px, int w, int h, float x, float y, UINT dpi, GlassColor col) {
    float s = scf(1.0f, dpi);
    glass::strokeRoundRect(px, w, h, x, y, x + 13 * s, y + 9 * s, 2 * s, 1.35f * s, col);
    glass::drawAaSegment(px, w, h, x + 3.5f * s, y + 11.5f * s, x + 9.5f * s, y + 11.5f * s, 1.4f * s, col);
}

void drawCheckGlyph(uint32_t* px, int w, int h, float x, float y, UINT dpi, GlassColor col) {
    float s = scf(1.0f, dpi);
    glass::drawAaSegment(px, w, h, x, y + 4 * s, x + 3 * s, y + 7 * s, 1.7f * s, col);
    glass::drawAaSegment(px, w, h, x + 3 * s, y + 7 * s, x + 9 * s, y + 1 * s, 1.7f * s, col);
}

void drawRefreshGlyph(uint32_t* px, int w, int h, float cx, float cy, UINT dpi, GlassColor col) {
    float s = scf(1.0f, dpi);
    glass::drawAaCircleRing(px, w, h, cx, cy, 4.6f * s, 1.3f * s, col);
    glass::drawAaSegment(px, w, h, cx + 2.2f * s, cy - 5.0f * s, cx + 5.2f * s, cy - 2.2f * s, 1.3f * s, col);
}

void drawGaugeGlyph(uint32_t* px, int w, int h, float cx, float cy, UINT dpi, GlassColor col) {
    float s = scf(1.0f, dpi);
    glass::drawAaCircleRing(px, w, h, cx, cy, 4.8f * s, 1.25f * s, col);
    glass::drawAaSegment(px, w, h, cx, cy, cx + 2.8f * s, cy - 2.4f * s, 1.3f * s, col);
}

void drawChipGlyph(uint32_t* px, int w, int h, float x, float y, UINT dpi, GlassColor col) {
    float s = scf(1.0f, dpi);
    glass::strokeRoundRect(px, w, h, x + 2 * s, y + 2 * s, x + 9 * s, y + 9 * s, 1.5f * s, 1.15f * s, col);
    glass::drawAaSegment(px, w, h, x, y + 4 * s, x + 2 * s, y + 4 * s, 1.15f * s, col);
    glass::drawAaSegment(px, w, h, x, y + 7 * s, x + 2 * s, y + 7 * s, 1.15f * s, col);
    glass::drawAaSegment(px, w, h, x + 9 * s, y + 4 * s, x + 11 * s, y + 4 * s, 1.15f * s, col);
    glass::drawAaSegment(px, w, h, x + 9 * s, y + 7 * s, x + 11 * s, y + 7 * s, 1.15f * s, col);
}

void drawSpeakerGlyph(uint32_t* px, int w, int h, float x, float y, UINT dpi, GlassColor col) {
    float s = scf(1.0f, dpi);
    glass::strokeRoundRect(px, w, h, x, y + 2.5f * s, x + 3.5f * s, y + 8.5f * s, 1 * s, 1.2f * s, col);
    glass::drawAaSegment(px, w, h, x + 3.5f * s, y + 2.5f * s, x + 7.5f * s, y + 0.5f * s, 1.3f * s, col);
    glass::drawAaSegment(px, w, h, x + 7.5f * s, y + 0.5f * s, x + 7.5f * s, y + 10.5f * s, 1.3f * s, col);
    glass::drawAaSegment(px, w, h, x + 7.5f * s, y + 10.5f * s, x + 3.5f * s, y + 8.5f * s, 1.3f * s, col);
    glass::drawAaSegment(px, w, h, x + 10 * s, y + 3 * s, x + 10 * s, y + 8 * s, 1.3f * s, col);
}

HICON createAppBadgeIcon(int sz = 32) {
    if (HICON resIcon = static_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, sz, sz, LR_DEFAULTCOLOR))) {
        return resIcon;
    }

    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = sz;
    bi.bV5Height = -sz;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    bi.bV5CSType    = LCS_sRGB;
    bi.bV5Intent    = LCS_GM_IMAGES;

    std::vector<uint8_t> maskZeros(static_cast<size_t>((sz + 15) / 16) * 2 * sz, 0);
    void* bits = nullptr;
    HDC screenDc = GetDC(nullptr);
    HBITMAP colorBmp = CreateDIBSection(screenDc, reinterpret_cast<BITMAPINFO*>(&bi),
                                        DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP maskBmp = CreateBitmap(sz, sz, 1, 1, maskZeros.data());
    ReleaseDC(nullptr, screenDc);
    if (!colorBmp || !maskBmp || !bits) {
        if (colorBmp) DeleteObject(colorBmp);
        if (maskBmp) DeleteObject(maskBmp);
        return LoadIconA(nullptr, IDI_APPLICATION);
    }

    uint32_t* px = static_cast<uint32_t*>(bits);
    std::memset(px, 0, static_cast<size_t>(sz) * sz * sizeof(uint32_t));
    glass::blitPremulArgbBilinear(px, sz, sz, kAppIcon48, kAppIconSize, kAppIconSize,
                                  0.0f, 0.0f, static_cast<float>(sz));

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = maskBmp;
    ii.hbmColor = colorBmp;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(colorBmp);
    DeleteObject(maskBmp);
    return icon ? icon : LoadIconA(nullptr, IDI_APPLICATION);
}

HFONT makeUiFont(int pt, UINT dpi, int weight) {
    return CreateFontW(-MulDiv(pt, static_cast<int>(dpi), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

void drawTextToCanvas(uint32_t* canvasPx, int cw, int ch,
                      HDC scratchDc, uint32_t* scratchPx, int sw, int sh,
                      DWriteTextEngine* dwEngine, int roleIdx,
                      const RECT& r, HFONT font, GlassColor col, const std::wstring& txt, UINT fmt) {
    if (!canvasPx || !scratchPx || txt.empty()) return;
    int bw = std::max(1, static_cast<int>(r.right - r.left));
    int bh = std::max(1, static_cast<int>(r.bottom - r.top));
    int boxW = std::min(bw, sw);
    int boxH = std::min(bh, sh);

    bool usedDWrite = (dwEngine && dwEngine->renderText(txt, roleIdx, fmt, boxW, boxH, scratchPx, sw));
    if (!usedDWrite) {
        for (int y = 0; y < boxH; ++y) {
            std::memset(scratchPx + static_cast<size_t>(y) * sw, 0, static_cast<size_t>(boxW) * 4);
        }
        SetBkMode(scratchDc, TRANSPARENT);
        SetTextColor(scratchDc, RGB(255, 255, 255));
        HGDIOBJ oldF = SelectObject(scratchDc, font);
        RECT localRc = { 0, 0, boxW, boxH };
        DrawTextW(scratchDc, txt.c_str(), -1, &localRc, fmt);
        SelectObject(scratchDc, oldF);
    }

    int dstX0 = std::max<int>(0, r.left);
    int dstY0 = std::max<int>(0, r.top);
    int dstX1 = std::min<int>(cw, r.left + boxW);
    int dstY1 = std::min<int>(ch, r.top + boxH);
    for (int dy = dstY0; dy < dstY1; ++dy) {
        int sy = dy - r.top;
        const uint32_t* sRow = scratchPx + static_cast<size_t>(sy) * sw;
        uint32_t* dRow = canvasPx + static_cast<size_t>(dy) * cw;
        for (int dx = dstX0; dx < dstX1; ++dx) {
            uint32_t sp = sRow[dx - r.left];
            uint32_t cov = 0;
            if (usedDWrite) {
                cov = (sp >> 24) & 0xFF;
            } else {
                if ((sp & 0x00FFFFFFu) == 0) continue;
                uint32_t sr = (sp >> 16) & 0xFF;
                uint32_t sg = (sp >> 8) & 0xFF;
                uint32_t sb = sp & 0xFF;
                cov = (sr + sg * 2 + sb) >> 2;
            }
            if (cov > 0) {
                glass::blendStraight(dRow[dx], col, static_cast<float>(cov) / 255.0f);
            }
        }
    }
}

struct SplashState {
    std::atomic<HWND> hwnd{nullptr};
    UINT dpi = 96;
    bool glass = false;
    bool done = false;
    float animPhase = 0.0f;
    HICON icon = nullptr;
    HFONT fTitle = nullptr;
    HFONT fSub = nullptr;
    DWriteTextEngine dwText;
    HDC canvasDc = nullptr;
    HBITMAP canvasBmp = nullptr;
    HGDIOBJ canvasOld = nullptr;
    uint32_t* canvasPx = nullptr;
    int cw = 0;
    int ch = 0;
    HDC scratchDc = nullptr;
    HBITMAP scratchBmp = nullptr;
    HGDIOBJ scratchOld = nullptr;
    uint32_t* scratchPx = nullptr;
    int sw = 0;
    int sh = 0;
    std::mutex mtx;
    std::string statusText = "Checking for updates...";
};

LRESULT CALLBACK splashWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SplashState* st = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        st = static_cast<SplashState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
    } else {
        st = reinterpret_cast<SplashState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!st) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_NCCALCSIZE:
            if (wParam == TRUE) return 0;
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_NCPAINT:
            return 0;
        case WM_NCACTIVATE:
            return TRUE;
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (wParam == TIMER_SPLASH_ANIM) {
                st->animPhase += 0.045f;
                if (st->animPhase > 1.0f) st->animPhase -= 1.0f;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_APP_SPLASH_STATUS:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_APP_SPLASH_DONE:
            st->done = true;
            st->hwnd.store(nullptr);
            KillTimer(hwnd, TIMER_SPLASH_ANIM);
            DestroyWindow(hwnd);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            HDC screenDc = BeginPaint(hwnd, &ps);
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            int w = std::max(1L, rc.right - rc.left);
            int h = std::max(1L, rc.bottom - rc.top);

            if (!st->canvasDc || st->cw != w || st->ch != h) {
                if (st->canvasDc) {
                    if (st->canvasOld) SelectObject(st->canvasDc, st->canvasOld);
                    DeleteDC(st->canvasDc);
                }
                if (st->canvasBmp) DeleteObject(st->canvasBmp);
                st->canvasDc = CreateCompatibleDC(screenDc);
                BITMAPINFO bi = {};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = w;
                bi.bmiHeader.biHeight = -h;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                void* bits = nullptr;
                st->canvasBmp = CreateDIBSection(screenDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
                st->canvasPx = static_cast<uint32_t*>(bits);
                st->canvasOld = st->canvasBmp ? SelectObject(st->canvasDc, st->canvasBmp) : nullptr;
                st->cw = w;
                st->ch = h;
            }
            if (!st->scratchDc || st->sw < w) {
                if (st->scratchDc) {
                    if (st->scratchOld) SelectObject(st->scratchDc, st->scratchOld);
                    DeleteDC(st->scratchDc);
                }
                if (st->scratchBmp) DeleteObject(st->scratchBmp);
                int sh = sc(40, st->dpi);
                st->scratchDc = CreateCompatibleDC(screenDc);
                BITMAPINFO bi = {};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = w;
                bi.bmiHeader.biHeight = -sh;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                void* bits = nullptr;
                st->scratchBmp = CreateDIBSection(screenDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
                st->scratchPx = static_cast<uint32_t*>(bits);
                st->scratchOld = st->scratchBmp ? SelectObject(st->scratchDc, st->scratchBmp) : nullptr;
                st->sw = w;
                st->sh = sh;
            }

            if (!st->canvasPx || !st->scratchPx) {
                EndPaint(hwnd, &ps);
                return 0;
            }

            std::string statusCopy;
            {
                std::lock_guard<std::mutex> lock(st->mtx);
                statusCopy = st->statusText;
            }

            glass::fillCanvasGlass(st->canvasPx, w, h, st->glass ? 65 : 255);
            glass::strokeRoundRect(st->canvasPx, w, h, 0.5f, 0.5f, w - 0.5f, h - 0.5f, scf(10, st->dpi), 1.0f, {255, 255, 255, 48});
            glass::blitPremulArgbBilinear(st->canvasPx, w, h, kAppIcon48, kAppIconSize, kAppIconSize,
                                          scf(13, st->dpi), scf(20, st->dpi), scf(28, st->dpi));
            drawTextToCanvas(st->canvasPx, w, h, st->scratchDc, st->scratchPx, st->sw, st->sh,
                             &st->dwText, 0,
                             { sc(46, st->dpi), sc(14, st->dpi), sc(170, st->dpi), sc(36, st->dpi) },
                             st->fTitle, {255, 255, 255, 255}, L"WinMirror",
                             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            drawTextToCanvas(st->canvasPx, w, h, st->scratchDc, st->scratchPx, st->sw, st->sh,
                             &st->dwText, 4,
                             { sc(170, st->dpi), sc(14, st->dpi), sc(312, st->dpi), sc(36, st->dpi) },
                             st->fSub, {161, 161, 170, 240}, L"karnajitop  \u2022  v" L"" WINMIRROR_VERSION,
                             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            drawTextToCanvas(st->canvasPx, w, h, st->scratchDc, st->scratchPx, st->sw, st->sh,
                             &st->dwText, 4,
                             { sc(46, st->dpi), sc(38, st->dpi), sc(312, st->dpi), sc(58, st->dpi) },
                             st->fSub, {212, 212, 216, 245}, utf8ToWide(statusCopy),
                             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            float tx0 = scf(16, st->dpi), tx1 = static_cast<float>(w) - scf(16, st->dpi);
            float ty0 = scf(72, st->dpi), ty1 = scf(76, st->dpi);
            glass::fillRoundRectGrad(st->canvasPx, w, h, tx0, ty0, tx1, ty1, scf(2, st->dpi),
                                     {36, 36, 44, 210}, {28, 28, 36, 210}, {0, 0, 0, 0}, 0.0f);
            float span = tx1 - tx0;
            float barW = span * 0.34f;
            float bx0 = tx0 + st->animPhase * (span - barW);
            glass::fillRoundRectGrad(st->canvasPx, w, h, bx0, ty0, bx0 + barW, ty1, scf(2, st->dpi),
                                     {239, 68, 68, 255}, {255, 92, 92, 255}, {0, 0, 0, 0}, 0.0f);

            BitBlt(screenDc, 0, 0, w, h, st->canvasDc, 0, 0, SRCCOPY);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE:
            st->done = true;
            st->hwnd.store(nullptr);
            KillTimer(hwnd, TIMER_SPLASH_ANIM);
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            st->hwnd.store(nullptr);
            KillTimer(hwnd, TIMER_SPLASH_ANIM);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

void WinMirrorGui::runPreLaunchUpdater(std::function<void(std::function<void(const std::string&)>)> task) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    std::wstring cls = WM_OBF_W(L"WMPreLaunchGlass");

    SplashState st;
    st.icon = createAppBadgeIcon();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = splashWndProc;
    wc.hInstance = hInst;
    wc.hIcon = st.icon;
    wc.hIconSm = st.icon;
    wc.hCursor = LoadCursorA(nullptr, IDC_APPSTARTING);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = cls.c_str();
    RegisterClassExW(&wc);

    HWND hSplash = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        cls.c_str(), L"WinMirror",
        WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, SPLASH_W, SPLASH_H,
        nullptr, nullptr, hInst, &st);
    st.hwnd.store(hSplash);

    if (!hSplash) {
        if (task) task([](const std::string&) {});
        if (st.icon) DestroyIcon(st.icon);
        return;
    }

    st.dpi = queryWindowDpi(hSplash);
    st.fTitle = makeUiFont(11, st.dpi, FW_SEMIBOLD);
    st.fSub = makeUiFont(9, st.dpi, FW_NORMAL);
    st.dwText.rebuildForDpi(st.dpi);
    st.glass = enableGlassBackdrop(hSplash);

    int w = sc(SPLASH_W, st.dpi);
    int h = sc(SPLASH_H, st.dpi);
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int posX = work.left + ((work.right - work.left) - w) / 2;
    int posY = work.top + ((work.bottom - work.top) - h) / 2;
    SetWindowPos(hSplash, HWND_TOPMOST, posX, posY, w, h, SWP_FRAMECHANGED);

    ShowWindow(hSplash, SW_SHOW);
    UpdateWindow(hSplash);
    SetTimer(hSplash, TIMER_SPLASH_ANIM, 30, nullptr);

    std::thread worker([&]() {
        wm_sec::hideCurrentThread();
        if (task) {
            task([&](const std::string& msg) {
                {
                    std::lock_guard<std::mutex> lock(st.mtx);
                    st.statusText = msg;
                }
                HWND hPost = st.hwnd.load();
                if (hPost) PostMessageW(hPost, WM_APP_SPLASH_STATUS, 0, 0);
            });
        }
        HWND hDone = st.hwnd.load();
        if (hDone) PostMessageW(hDone, WM_APP_SPLASH_DONE, 0, 0);
    });

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (worker.joinable()) worker.join();
    if (st.canvasDc) {
        if (st.canvasOld) SelectObject(st.canvasDc, st.canvasOld);
        DeleteDC(st.canvasDc);
    }
    if (st.canvasBmp) DeleteObject(st.canvasBmp);
    if (st.scratchDc) {
        if (st.scratchOld) SelectObject(st.scratchDc, st.scratchOld);
        DeleteDC(st.scratchDc);
    }
    if (st.scratchBmp) DeleteObject(st.scratchBmp);
    if (st.fTitle) DeleteObject(st.fTitle);
    if (st.fSub) DeleteObject(st.fSub);
    if (st.icon) DestroyIcon(st.icon);
    UnregisterClassW(cls.c_str(), hInst);
    wm_sec::wipeWString(cls);
}

WinMirrorGui::WinMirrorGui() = default;

WinMirrorGui::~WinMirrorGui() {
    removeTrayIcon();
    releaseBitmaps();
    delete m_dwText;
    m_dwText = nullptr;
    if (m_appIcon) { DestroyIcon(m_appIcon); m_appIcon = nullptr; }
    if (m_appIconSm) { DestroyIcon(m_appIconSm); m_appIconSm = nullptr; }
    if (m_fontTitle) { DeleteObject(m_fontTitle); m_fontTitle = nullptr; }
    if (m_fontRow) { DeleteObject(m_fontRow); m_fontRow = nullptr; }
    if (m_fontLabel) { DeleteObject(m_fontLabel); m_fontLabel = nullptr; }
    if (m_fontBody) { DeleteObject(m_fontBody); m_fontBody = nullptr; }
    if (m_fontSmall) { DeleteObject(m_fontSmall); m_fontSmall = nullptr; }
}

void WinMirrorGui::releaseBitmaps() {
    if (m_canvasDc) {
        if (m_canvasOldBmp) SelectObject(m_canvasDc, m_canvasOldBmp);
        DeleteDC(m_canvasDc);
        m_canvasDc = nullptr;
        m_canvasOldBmp = nullptr;
    }
    if (m_canvasBmp) { DeleteObject(m_canvasBmp); m_canvasBmp = nullptr; }
    m_canvasPx = nullptr;
    m_canvasW = m_canvasH = 0;

    if (m_scratchDc) {
        if (m_scratchOldBmp) SelectObject(m_scratchDc, m_scratchOldBmp);
        DeleteDC(m_scratchDc);
        m_scratchDc = nullptr;
        m_scratchOldBmp = nullptr;
    }
    if (m_scratchBmp) { DeleteObject(m_scratchBmp); m_scratchBmp = nullptr; }
    m_scratchPx = nullptr;
    m_scratchW = m_scratchH = 0;
}

void WinMirrorGui::ensureCanvas(HDC refDc, int w, int h) {
    w = std::max(1, w);
    h = std::max(1, h);
    if (m_canvasDc && m_canvasW == w && m_canvasH == h && m_canvasPx) return;
    if (m_canvasDc) {
        if (m_canvasOldBmp) SelectObject(m_canvasDc, m_canvasOldBmp);
        DeleteDC(m_canvasDc);
        m_canvasDc = nullptr;
    }
    if (m_canvasBmp) { DeleteObject(m_canvasBmp); m_canvasBmp = nullptr; }

    m_canvasDc = CreateCompatibleDC(refDc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    m_canvasBmp = CreateDIBSection(refDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    m_canvasPx = static_cast<uint32_t*>(bits);
    m_canvasOldBmp = m_canvasBmp ? SelectObject(m_canvasDc, m_canvasBmp) : nullptr;
    m_canvasW = w;
    m_canvasH = h;
}

void WinMirrorGui::ensureTextScratch(HDC refDc, int w, int h) {
    w = std::max(64, w);
    h = std::max(32, h);
    if (m_scratchDc && m_scratchW >= w && m_scratchH >= h && m_scratchPx) return;
    if (m_scratchDc) {
        if (m_scratchOldBmp) SelectObject(m_scratchDc, m_scratchOldBmp);
        DeleteDC(m_scratchDc);
        m_scratchDc = nullptr;
    }
    if (m_scratchBmp) { DeleteObject(m_scratchBmp); m_scratchBmp = nullptr; }

    int allocW = std::max(w, m_canvasW);
    int allocH = std::max(h, 96);
    m_scratchDc = CreateCompatibleDC(refDc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = allocW;
    bi.bmiHeader.biHeight = -allocH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    m_scratchBmp = CreateDIBSection(refDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    m_scratchPx = static_cast<uint32_t*>(bits);
    m_scratchOldBmp = m_scratchBmp ? SelectObject(m_scratchDc, m_scratchBmp) : nullptr;
    m_scratchW = allocW;
    m_scratchH = allocH;
}

void WinMirrorGui::drawTextGlass(const RECT& r, HFONT font, GlassColor col, const std::wstring& txt, UINT fmt) {
    int roleIdx = 4; // Small
    if (font == m_fontTitle) roleIdx = 0;
    else if (font == m_fontRow) roleIdx = 1;
    else if (font == m_fontLabel) roleIdx = 2;
    else if (font == m_fontBody) roleIdx = 3;
    drawTextToCanvas(m_canvasPx, m_canvasW, m_canvasH,
                     m_scratchDc, m_scratchPx, m_scratchW, m_scratchH,
                     m_dwText, roleIdx, r, font, col, txt, fmt);
}

void WinMirrorGui::setActions(std::function<void()> onQuit,
                              std::function<void(const AirPlayTarget&)> onSelectTarget,
                              std::function<void(const std::string&)> onConnectIp,
                              std::function<void()> onRefresh,
                              std::function<void()> onCheckUpdate) {
    m_onQuit = std::move(onQuit);
    m_onSelectTarget = std::move(onSelectTarget);
    m_onConnectIp = std::move(onConnectIp);
    m_onRefresh = std::move(onRefresh);
    m_onCheckUpdate = std::move(onCheckUpdate);
}

void WinMirrorGui::rebuildFonts() {
    if (m_fontTitle) DeleteObject(m_fontTitle);
    if (m_fontRow) DeleteObject(m_fontRow);
    if (m_fontLabel) DeleteObject(m_fontLabel);
    if (m_fontBody) DeleteObject(m_fontBody);
    if (m_fontSmall) DeleteObject(m_fontSmall);

    m_fontTitle = makeUiFont(11, m_dpi, FW_SEMIBOLD);
    m_fontRow   = makeUiFont(10, m_dpi, FW_SEMIBOLD);
    m_fontLabel = makeUiFont(8,  m_dpi, FW_SEMIBOLD);
    m_fontBody  = makeUiFont(9,  m_dpi, FW_NORMAL);
    m_fontSmall = makeUiFont(9,  m_dpi, FW_NORMAL);

    if (!m_dwText) m_dwText = new DWriteTextEngine();
    m_dwText->rebuildForDpi(m_dpi);
}

bool WinMirrorGui::createAndShow() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    std::wstring clsName = WM_OBF_W(L"WMAcrylicMirrorCard");

    m_appIcon = createAppBadgeIcon(std::max(48, GetSystemMetrics(SM_CXICON)));
    m_appIconSm = createAppBadgeIcon(std::max(16, GetSystemMetrics(SM_CXSMICON)));

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProcThunk;
    wc.hInstance = hInst;
    wc.hIcon = m_appIcon;
    wc.hIconSm = m_appIconSm;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = clsName.c_str();
    RegisterClassExW(&wc);

    const DWORD style = WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU;
    const DWORD exStyle = WS_EX_APPWINDOW;

    HWND hMain = CreateWindowExW(
        exStyle, clsName.c_str(), L"Screen Mirroring", style,
        CW_USEDEFAULT, CW_USEDEFAULT, BASE_W, BASE_H,
        nullptr, nullptr, hInst, this);
    wm_sec::wipeWString(clsName);
    m_hwnd.store(hMain);
    if (!hMain) return false;

    SendMessageW(hMain, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(m_appIcon));
    SendMessageW(hMain, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(m_appIconSm));

    m_dpi = queryWindowDpi(hMain);
    rebuildFonts();
    m_glassBlurActive = enableGlassBackdrop(hMain);

    int winW = sc(BASE_W, m_dpi);
    int winH = sc(BASE_H, m_dpi);
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int posX = work.left + ((work.right - work.left) - winW) / 2;
    int posY = work.top + ((work.bottom - work.top) - winH) / 2;
    SetWindowPos(hMain, nullptr, posX, posY, winW, winH, SWP_NOZORDER | SWP_FRAMECHANGED);

    addTrayIcon();
    ShowWindow(hMain, SW_SHOW);
    UpdateWindow(hMain);
    return true;
}

void WinMirrorGui::updateStatus(const WinMirrorStatus& st) {
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_status = st;
    }
    HWND h = m_hwnd.load();
    if (h) {
        PostMessageW(h, WM_APP_REFRESH, 0, 0);
    }
}

void WinMirrorGui::setStatusBanner(const std::string& msg) {
    if (msg.empty()) return;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_status.statusBanner = msg;
    }
    HWND h = m_hwnd.load();
    if (h) {
        PostMessageW(h, WM_APP_REFRESH, 0, 0);
    }
}

int WinMirrorGui::runMessageLoop() {
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void WinMirrorGui::requestClose() {
    HWND h = m_hwnd.load();
    if (h) {
        PostMessageW(h, WM_CLOSE, 0, 0);
    }
}

int WinMirrorGui::hitTestButton(POINT pt) const {
    for (const auto& item : buildControlRects(m_dpi)) {
        if (PtInRect(&item.rc, pt)) return item.id;
    }
    return 0;
}

int WinMirrorGui::hitTestReceiverRow(POINT pt, size_t maxRows) const {
    size_t count = std::min(maxRows, MAX_VISIBLE_ROWS);
    for (size_t i = 0; i < count; ++i) {
        RECT r = receiverRowRect(static_cast<int>(i), m_dpi);
        if (PtInRect(&r, pt)) return static_cast<int>(i);
    }
    return -1;
}

void WinMirrorGui::invokeButtonAction(int btnId) {
    if (btnId == ID_BTN_MINIMIZE) {
        hideToTray();
    } else if (btnId == ID_BTN_CLOSE) {
        requestClose();
    } else if (btnId == ID_BTN_CONNECT) {
        if (!m_manualIp.empty() && m_onConnectIp) {
            m_onConnectIp(m_manualIp);
        } else {
            AirPlayTarget firstDirect;
            bool found = false;
            {
                std::lock_guard<std::mutex> lock(m_mtx);
                for (const auto& t : m_status.receivers) {
                    if (t.isDirectObs || t.isLaptop) {
                        firstDirect = t;
                        found = true;
                        break;
                    }
                }
            }
            if (found && m_onSelectTarget) {
                m_onSelectTarget(firstDirect);
            } else if (m_onRefresh) {
                m_onRefresh();
            }
        }
    } else if (btnId == ID_BTN_SCAN && m_onRefresh) {
        m_onRefresh();
    }
}

void WinMirrorGui::appendIpCharsFromClipboard() {
    if (!OpenClipboard(m_hwnd)) return;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        const wchar_t* wp = static_cast<const wchar_t*>(GlobalLock(hData));
        if (wp) {
            std::string clip = wideToUtf8(wp);
            GlobalUnlock(hData);
            for (char c : clip) {
                if ((c >= '0' && c <= '9') || c == '.' || c == ':') {
                    if (m_manualIp.size() < 28) m_manualIp.push_back(c);
                }
            }
        }
    }
    CloseClipboard();
}

void WinMirrorGui::addTrayIcon() {
    if (m_trayAdded || !m_hwnd) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = m_appIconSm ? m_appIconSm : m_appIcon;
    std::wcsncpy(nid.szTip, L"WinMirror \u2014 by karnajitop", ARRAYSIZE(nid.szTip) - 1);
    m_trayAdded = (Shell_NotifyIconW(NIM_ADD, &nid) == TRUE);
}

void WinMirrorGui::removeTrayIcon() {
    if (!m_trayAdded || !m_hwnd) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    m_trayAdded = false;
}

void WinMirrorGui::hideToTray() {
    if (!m_hwnd) return;
    addTrayIcon();
    ShowWindow(m_hwnd, SW_HIDE);
}

void WinMirrorGui::showFromTray() {
    if (!m_hwnd) return;
    ShowWindow(m_hwnd, SW_SHOW);
    ShowWindow(m_hwnd, SW_RESTORE);
    SetForegroundWindow(m_hwnd);
}

void WinMirrorGui::showTrayContextMenu() {
    if (!m_hwnd) return;
    POINT pt = {};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, L"Open Screen Mirroring");
    AppendMenuW(menu, MF_STRING, ID_TRAY_UPDATE, L"Check for Updates (v" L"" WINMIRROR_VERSION L")");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT, L"Quit");

    SetForegroundWindow(m_hwnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                              pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == ID_TRAY_OPEN) {
        showFromTray();
    } else if (cmd == ID_TRAY_UPDATE) {
        if (m_onCheckUpdate) m_onCheckUpdate();
    } else if (cmd == ID_TRAY_QUIT) {
        requestClose();
    }
}

void WinMirrorGui::paintWindow(HWND hwnd) {
    PAINTSTRUCT ps = {};
    HDC screenDc = BeginPaint(hwnd, &ps);

    RECT client = {};
    GetClientRect(hwnd, &client);
    int cw = std::max(1L, client.right - client.left);
    int ch = std::max(1L, client.bottom - client.top);

    ensureCanvas(screenDc, cw, ch);
    ensureTextScratch(screenDc, cw, sc(72, m_dpi));
    if (!m_canvasPx || !m_scratchPx) {
        EndPaint(hwnd, &ps);
        return;
    }

    WinMirrorStatus snap;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        snap = m_status;
    }

    size_t bgPixels = static_cast<size_t>(cw) * static_cast<size_t>(ch);
    if (m_baseBgW != cw || m_baseBgH != ch || m_baseBgGlass != m_glassBlurActive || m_baseBg.size() != bgPixels) {
        m_baseBg.resize(bgPixels);
        // 30% obsidian tint (alpha=65) + subtle DWM Acrylic tint => ~70% transparent frosted glass
        glass::fillCanvasGlass(m_baseBg.data(), cw, ch, m_glassBlurActive ? 65 : 255);
        glass::strokeRoundRect(m_baseBg.data(), cw, ch, 0.5f, 0.5f, cw - 0.5f, ch - 0.5f,
                               scf(10, m_dpi), 1.0f, {255, 255, 255, 48});
        m_baseBgW = cw;
        m_baseBgH = ch;
        m_baseBgGlass = m_glassBlurActive;
    }
    std::memcpy(m_canvasPx, m_baseBg.data(), bgPixels * sizeof(uint32_t));

    const GlassColor cWhite = {255, 255, 255, 255};
    const GlassColor cSilver = {212, 212, 216, 250};
    const GlassColor cMuted = {161, 161, 170, 238};
    const GlassColor cRed   = {239, 68, 68, 255};

    // Header: WinMirror App Logo Badge + Google-style Crisp Typography + Status Pill + Chrome
    glass::blitPremulArgbBilinear(m_canvasPx, cw, ch, kAppIcon48, kAppIconSize, kAppIconSize,
                                  scf(13, m_dpi), scf(16, m_dpi), scf(28, m_dpi));
    drawTextGlass({ sc(46, m_dpi), sc(12, m_dpi), sc(244, m_dpi), sc(33, m_dpi) },
                  m_fontTitle, cWhite, L"Screen Mirroring",
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    drawTextGlass({ sc(46, m_dpi), sc(32, m_dpi), sc(244, m_dpi), sc(49, m_dpi) },
                  m_fontSmall, cMuted, L"AirPlay  \u2022  by karnajitop",
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    glass::fillRoundRectGrad(m_canvasPx, cw, ch,
                             scf(248, m_dpi), scf(16, m_dpi), scf(336, m_dpi), scf(40, m_dpi),
                             scf(12, m_dpi),
                             snap.clients > 0 ? GlassColor{58, 12, 18, 195} : GlassColor{20, 20, 26, 130},
                             snap.clients > 0 ? GlassColor{38, 9, 14, 195} : GlassColor{14, 14, 20, 130},
                             snap.clients > 0 ? GlassColor{239, 68, 68, 215} : GlassColor{255, 255, 255, 56}, 1.0f);
    glass::fillAaCircle(m_canvasPx, cw, ch, scf(260, m_dpi), scf(28, m_dpi), scf(3.4f, m_dpi),
                        snap.clients > 0 ? cRed : (snap.streamingReady ? cWhite : cMuted));
    wchar_t pillTxt[32] = {};
    if (snap.clients > 0) {
        std::swprintf(pillTxt, ARRAYSIZE(pillTxt), L"Live \u2022 %.0f", snap.actualFps);
    } else if (snap.streamingReady) {
        std::swprintf(pillTxt, ARRAYSIZE(pillTxt), L"Ready");
    } else {
        std::swprintf(pillTxt, ARRAYSIZE(pillTxt), L"Init...");
    }
    drawTextGlass({ sc(269, m_dpi), sc(16, m_dpi), sc(332, m_dpi), sc(40, m_dpi) },
                  m_fontLabel, cWhite, pillTxt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const auto ctrls = buildControlRects(m_dpi);
    auto rectOf = [&](int id) -> RECT {
        for (const auto& c : ctrls) if (c.id == id) return c.rc;
        return {};
    };

    // Custom Minimal Black/Red/White Chrome Buttons (- and X)
    RECT rcMin = rectOf(ID_BTN_MINIMIZE);
    bool minHv = (m_hoverBtnId == ID_BTN_MINIMIZE);
    glass::fillRoundRectGrad(m_canvasPx, cw, ch,
                             static_cast<float>(rcMin.left), static_cast<float>(rcMin.top),
                             static_cast<float>(rcMin.right), static_cast<float>(rcMin.bottom),
                             scf(7, m_dpi),
                             minHv ? GlassColor{42, 42, 50, 175} : GlassColor{20, 20, 26, 124},
                             minHv ? GlassColor{34, 34, 42, 175} : GlassColor{14, 14, 20, 124},
                             minHv ? GlassColor{255, 255, 255, 92} : GlassColor{255, 255, 255, 50}, 1.0f);
    glass::drawAaSegment(m_canvasPx, cw, ch,
                         static_cast<float>(rcMin.left + sc(7, m_dpi)),
                         static_cast<float>((rcMin.top + rcMin.bottom) / 2),
                         static_cast<float>(rcMin.right - sc(7, m_dpi)),
                         static_cast<float>((rcMin.top + rcMin.bottom) / 2),
                         scf(1.5f, m_dpi), cWhite);

    RECT rcClose = rectOf(ID_BTN_CLOSE);
    bool closeHv = (m_hoverBtnId == ID_BTN_CLOSE);
    glass::fillRoundRectGrad(m_canvasPx, cw, ch,
                             static_cast<float>(rcClose.left), static_cast<float>(rcClose.top),
                             static_cast<float>(rcClose.right), static_cast<float>(rcClose.bottom),
                             scf(7, m_dpi),
                             closeHv ? GlassColor{220, 38, 38, 240} : GlassColor{20, 20, 26, 124},
                             closeHv ? GlassColor{185, 28, 28, 240} : GlassColor{14, 14, 20, 124},
                             closeHv ? GlassColor{252, 165, 165, 210} : GlassColor{255, 255, 255, 50}, 1.0f);
    glass::drawAaSegment(m_canvasPx, cw, ch,
                         static_cast<float>(rcClose.left + scf(7.5f, m_dpi)),
                         static_cast<float>(rcClose.top + scf(7.5f, m_dpi)),
                         static_cast<float>(rcClose.right - scf(7.5f, m_dpi)),
                         static_cast<float>(rcClose.bottom - scf(7.5f, m_dpi)),
                         scf(1.5f, m_dpi), cWhite);
    glass::drawAaSegment(m_canvasPx, cw, ch,
                         static_cast<float>(rcClose.right - scf(7.5f, m_dpi)),
                         static_cast<float>(rcClose.top + scf(7.5f, m_dpi)),
                         static_cast<float>(rcClose.left + scf(7.5f, m_dpi)),
                         static_cast<float>(rcClose.bottom - scf(7.5f, m_dpi)),
                         scf(1.5f, m_dpi), cWhite);

    // Compact Obsidian Receiver Card Container
    glass::fillRoundRectGrad(m_canvasPx, cw, ch,
                             scf(16, m_dpi), scf(60, m_dpi), scf(396, m_dpi), scf(210, m_dpi),
                             scf(14, m_dpi),
                             {12, 12, 16, 86}, {8, 8, 12, 86},
                             {255, 255, 255, 52}, 1.0f);

    if (snap.receivers.empty()) {
        drawLaptopGlyph(m_canvasPx, cw, ch, scf(194, m_dpi), scf(96, m_dpi), m_dpi, cSilver);
        drawTextGlass({ sc(28, m_dpi), sc(122, m_dpi), sc(384, m_dpi), sc(146, m_dpi) },
                      m_fontRow, cWhite, L"Looking for AirPlay devices...",
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        drawTextGlass({ sc(28, m_dpi), sc(146, m_dpi), sc(384, m_dpi), sc(168, m_dpi) },
                      m_fontSmall, cMuted, L"Open OBS or WinMirror on Mac",
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        size_t visibleCount = std::min(snap.receivers.size(), MAX_VISIBLE_ROWS);
        for (size_t i = 0; i < visibleCount; ++i) {
            const auto& dev = snap.receivers[i];
            RECT row = receiverRowRect(static_cast<int>(i), m_dpi);
            bool hovered = (m_hoverRowIdx == static_cast<int>(i));

            if (dev.isConnected) {
                glass::fillRoundRectGrad(
                    m_canvasPx, cw, ch,
                    static_cast<float>(row.left), static_cast<float>(row.top),
                    static_cast<float>(row.right), static_cast<float>(row.bottom),
                    scf(11, m_dpi),
                    hovered ? GlassColor{62, 14, 20, 195} : GlassColor{48, 11, 16, 178},
                    hovered ? GlassColor{40, 10, 15, 195} : GlassColor{30, 8, 12, 178},
                    {239, 68, 68, 225}, 1.25f);
            } else {
                glass::fillRoundRectGrad(
                    m_canvasPx, cw, ch,
                    static_cast<float>(row.left), static_cast<float>(row.top),
                    static_cast<float>(row.right), static_cast<float>(row.bottom),
                    scf(11, m_dpi),
                    hovered ? GlassColor{36, 20, 24, 152} : GlassColor{18, 18, 24, 120},
                    hovered ? GlassColor{26, 15, 19, 152} : GlassColor{14, 14, 20, 120},
                    hovered ? GlassColor{239, 68, 68, 185} : GlassColor{255, 255, 255, 44}, 1.0f);
            }

            GlassColor iconCol = dev.isConnected ? cRed : (dev.isDirectObs ? cWhite : cSilver);
            if (dev.isLaptop) {
                drawLaptopGlyph(m_canvasPx, cw, ch,
                                static_cast<float>(row.left + sc(14, m_dpi)),
                                static_cast<float>(row.top + sc(22, m_dpi)), m_dpi, iconCol);
            } else {
                drawTvGlyph(m_canvasPx, cw, ch,
                            static_cast<float>(row.left + sc(14, m_dpi)),
                            static_cast<float>(row.top + sc(22, m_dpi)), m_dpi, iconCol);
            }

            drawTextGlass({ row.left + sc(50, m_dpi), row.top + sc(10, m_dpi),
                            row.right - sc(90, m_dpi), row.top + sc(33, m_dpi) },
                          m_fontRow, cWhite, utf8ToWide(dev.name),
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            wchar_t subLine[96] = {};
            if (dev.isConnected) {
                std::swprintf(subLine, ARRAYSIZE(subLine), L"%ux%u \u2022 %ls \u2022 %ls",
                              snap.width ? snap.width : 1920,
                              snap.height ? snap.height : 1080,
                              snap.isHevc ? L"HEVC" : L"H.264",
                              snap.audioActive ? L"Audio" : L"Muted");
            } else if (dev.isDirectObs) {
                std::swprintf(subLine, ARRAYSIZE(subLine), L"OBS Receiver \u2022 Ready");
            } else {
                std::swprintf(subLine, ARRAYSIZE(subLine), L"AirPlay Receiver \u2022 Ready");
            }
            drawTextGlass({ row.left + sc(50, m_dpi), row.top + sc(33, m_dpi),
                            row.right - sc(90, m_dpi), row.top + sc(53, m_dpi) },
                          m_fontSmall, cMuted, subLine,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            if (dev.isConnected) {
                float bx0 = static_cast<float>(row.right - sc(82, m_dpi));
                float by0 = static_cast<float>(row.top + sc(18, m_dpi));
                float bx1 = static_cast<float>(row.right - sc(12, m_dpi));
                float by1 = static_cast<float>(row.bottom - sc(18, m_dpi));
                glass::fillRoundRectGrad(m_canvasPx, cw, ch, bx0, by0, bx1, by1, scf(12, m_dpi),
                                         {220, 38, 38, 245}, {185, 28, 28, 245}, {252, 165, 165, 190}, 1.0f);
                drawCheckGlyph(m_canvasPx, cw, ch, bx0 + scf(9, m_dpi), by0 + scf(9, m_dpi), m_dpi, cWhite);
                drawTextGlass({ static_cast<LONG>(bx0) + sc(22, m_dpi), static_cast<LONG>(by0),
                                static_cast<LONG>(bx1) - sc(6, m_dpi), static_cast<LONG>(by1) },
                              m_fontLabel, cWhite, L"Active", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            } else if (dev.isDirectObs) {
                float bx0 = static_cast<float>(row.right - sc(58, m_dpi));
                float by0 = static_cast<float>(row.top + sc(19, m_dpi));
                float bx1 = static_cast<float>(row.right - sc(12, m_dpi));
                float by1 = static_cast<float>(row.bottom - sc(19, m_dpi));
                glass::fillRoundRectGrad(m_canvasPx, cw, ch, bx0, by0, bx1, by1, scf(9, m_dpi),
                                         {46, 14, 18, 228}, {34, 10, 14, 228}, {239, 68, 68, 175}, 1.0f);
                drawTextGlass({ static_cast<LONG>(bx0), static_cast<LONG>(by0),
                                static_cast<LONG>(bx1), static_cast<LONG>(by1) },
                              m_fontLabel, cWhite, L"OBS", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                float bx0 = static_cast<float>(row.right - sc(72, m_dpi));
                float by0 = static_cast<float>(row.top + sc(19, m_dpi));
                float bx1 = static_cast<float>(row.right - sc(12, m_dpi));
                float by1 = static_cast<float>(row.bottom - sc(19, m_dpi));
                glass::fillRoundRectGrad(m_canvasPx, cw, ch, bx0, by0, bx1, by1, scf(9, m_dpi),
                                         {30, 30, 38, 215}, {24, 24, 30, 215}, {255, 255, 255, 36}, 1.0f);
                drawTextGlass({ static_cast<LONG>(bx0), static_cast<LONG>(by0),
                                static_cast<LONG>(bx1), static_cast<LONG>(by1) },
                              m_fontLabel, cSilver, L"AirPlay", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
    }

    // 1) Compact Obsidian IP Input Box
    RECT rcIp = rectOf(ID_IP_BOX);
    glass::fillRoundRectGrad(m_canvasPx, cw, ch,
                             static_cast<float>(rcIp.left), static_cast<float>(rcIp.top),
                             static_cast<float>(rcIp.right), static_cast<float>(rcIp.bottom),
                             scf(10, m_dpi),
                             {16, 16, 22, 122}, {12, 12, 16, 122},
                             m_ipFocused ? GlassColor{239, 68, 68, 225}
                                         : (m_hoverBtnId == ID_IP_BOX ? GlassColor{255, 255, 255, 96}
                                                                      : GlassColor{255, 255, 255, 52}),
                             m_ipFocused ? 1.3f : 1.0f);
    drawMonitorGlyph(m_canvasPx, cw, ch,
                     static_cast<float>(rcIp.left + sc(11, m_dpi)),
                     static_cast<float>(rcIp.top + sc(12, m_dpi)), m_dpi,
                     m_ipFocused ? cRed : cMuted);

    std::wstring ipLabel;
    GlassColor ipTextCol = cWhite;
    if (!m_manualIp.empty()) {
        ipLabel = utf8ToWide(m_manualIp);
        if (m_ipFocused && m_caretOn) ipLabel += L"|";
    } else if (m_ipFocused) {
        ipLabel = m_caretOn ? L"| Type host to connect..." : L"  Type host to connect...";
        ipTextCol = cMuted;
    } else {
        std::string autoName;
        for (const auto& t : snap.receivers) {
            if (t.isDirectObs) { autoName = t.name; break; }
        }
        ipLabel = !autoName.empty() ? (L"Ready: " + utf8ToWide(autoName)) : L"Type host to connect...";
        ipTextCol = cMuted;
    }
    drawTextGlass({ rcIp.left + sc(31, m_dpi), rcIp.top, rcIp.right - sc(10, m_dpi), rcIp.bottom },
                  m_fontBody, ipTextCol, ipLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // 2) Signature Crimson Connect Pill (with fixed AA white Play triangle)
    RECT rcConn = rectOf(ID_BTN_CONNECT);
    bool connHv = (m_hoverBtnId == ID_BTN_CONNECT);
    bool connDn = (m_pressedBtnId == ID_BTN_CONNECT);
    glass::fillRoundRectGrad(m_canvasPx, cw, ch,
                             static_cast<float>(rcConn.left), static_cast<float>(rcConn.top),
                             static_cast<float>(rcConn.right), static_cast<float>(rcConn.bottom),
                             scf(10, m_dpi),
                             connDn ? GlassColor{175, 24, 28, 250} : (connHv ? GlassColor{244, 52, 58, 252} : GlassColor{225, 29, 40, 248}),
                             connDn ? GlassColor{140, 18, 22, 250} : (connHv ? GlassColor{205, 32, 38, 252} : GlassColor{180, 22, 30, 248}),
                             {254, 202, 202, 130}, 1.0f);
    glass::fillAaPlayTriangle(m_canvasPx, cw, ch,
                              static_cast<float>(rcConn.left + sc(20, m_dpi)),
                              static_cast<float>((rcConn.top + rcConn.bottom) / 2),
                              scf(8.5f, m_dpi), cWhite);
    drawTextGlass({ rcConn.left + sc(31, m_dpi), rcConn.top, rcConn.right - sc(8, m_dpi), rcConn.bottom },
                  m_fontBody, cWhite, L"Connect", DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 3) Obsidian Scan Pill
    RECT rcScan = rectOf(ID_BTN_SCAN);
    bool scanHv = (m_hoverBtnId == ID_BTN_SCAN);
    glass::fillRoundRectGrad(m_canvasPx, cw, ch,
                             static_cast<float>(rcScan.left), static_cast<float>(rcScan.top),
                             static_cast<float>(rcScan.right), static_cast<float>(rcScan.bottom),
                             scf(10, m_dpi),
                             scanHv ? GlassColor{42, 24, 28, 168} : GlassColor{20, 20, 26, 128},
                             scanHv ? GlassColor{32, 18, 22, 168} : GlassColor{14, 14, 20, 128},
                             scanHv ? GlassColor{239, 68, 68, 185} : GlassColor{255, 255, 255, 52}, 1.0f);
    drawRefreshGlyph(m_canvasPx, cw, ch,
                     static_cast<float>(rcScan.left + sc(16, m_dpi)),
                     static_cast<float>((rcScan.top + rcScan.bottom) / 2), m_dpi, cWhite);
    drawTextGlass({ rcScan.left + sc(26, m_dpi), rcScan.top, rcScan.right - sc(6, m_dpi), rcScan.bottom },
                  m_fontBody, cWhite, L"Scan", DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Compact Telemetry Spec Strip
    int specY0 = sc(269, m_dpi), specY1 = sc(291, m_dpi);
    drawChipGlyph(m_canvasPx, cw, ch, scf(18, m_dpi), scf(275, m_dpi), m_dpi, cMuted);
    drawTextGlass({ sc(34, m_dpi), specY0, sc(90, m_dpi), specY1 },
                  m_fontSmall, cWhite, snap.isHevc ? L"HEVC" : L"H.264", DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    glass::drawAaSegment(m_canvasPx, cw, ch, scf(94, m_dpi), scf(273, m_dpi), scf(94, m_dpi), scf(287, m_dpi), 1.0f, {255, 255, 255, 32});
    drawGaugeGlyph(m_canvasPx, cw, ch, scf(108, m_dpi), scf(280, m_dpi), m_dpi, cMuted);
    wchar_t fpsSpec[32] = {};
    std::swprintf(fpsSpec, ARRAYSIZE(fpsSpec), L"%.0f FPS", snap.clients > 0 ? snap.actualFps : static_cast<float>(snap.targetFps));
    drawTextGlass({ sc(120, m_dpi), specY0, sc(180, m_dpi), specY1 },
                  m_fontSmall, cWhite, fpsSpec, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    glass::drawAaSegment(m_canvasPx, cw, ch, scf(184, m_dpi), scf(273, m_dpi), scf(184, m_dpi), scf(287, m_dpi), 1.0f, {255, 255, 255, 32});
    glass::fillAaCircle(m_canvasPx, cw, ch, scf(198, m_dpi), scf(280, m_dpi), scf(3.0f, m_dpi), snap.clients > 0 ? cRed : cMuted);
    wchar_t mbpsSpec[40] = {};
    if (snap.clients > 0) {
        std::swprintf(mbpsSpec, ARRAYSIZE(mbpsSpec), L"%.1f Mbps", snap.actualMbps);
    } else {
        std::swprintf(mbpsSpec, ARRAYSIZE(mbpsSpec), L"%u Mbps", snap.targetMbps ? snap.targetMbps : 200);
    }
    drawTextGlass({ sc(208, m_dpi), specY0, sc(294, m_dpi), specY1 },
                  m_fontSmall, cWhite, mbpsSpec, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    glass::drawAaSegment(m_canvasPx, cw, ch, scf(300, m_dpi), scf(273, m_dpi), scf(300, m_dpi), scf(287, m_dpi), 1.0f, {255, 255, 255, 32});
    drawSpeakerGlyph(m_canvasPx, cw, ch, scf(314, m_dpi), scf(275, m_dpi), m_dpi, snap.audioActive ? cWhite : cMuted);
    drawTextGlass({ sc(330, m_dpi), specY0, sc(396, m_dpi), specY1 },
                  m_fontSmall, snap.audioActive ? cWhite : cMuted,
                  snap.audioActive ? L"Audio" : L"Muted", DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Clean Crisp Footer Status Line
    glass::fillAaCircle(m_canvasPx, cw, ch, scf(22, m_dpi), scf(309, m_dpi), scf(3.6f, m_dpi),
                        snap.clients > 0 ? cRed : cWhite);
    drawTextGlass({ sc(33, m_dpi), sc(297, m_dpi), sc(396, m_dpi), sc(321, m_dpi) },
                  m_fontSmall, snap.clients > 0 ? cWhite : cSilver,
                  utf8ToWide(snap.statusBanner),
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    BitBlt(screenDc, 0, 0, cw, ch, m_canvasDc, 0, 0, SRCCOPY);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK WinMirrorGui::wndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WinMirrorGui* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<WinMirrorGui*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<WinMirrorGui*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->handleMessage(hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT WinMirrorGui::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wParam == TRUE) return 0;
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_NCPAINT:
            return 0;
        case WM_NCACTIVATE:
            return TRUE;
        case WM_NCHITTEST: {
            POINT pt = { static_cast<int>(static_cast<short>(LOWORD(lParam))),
                         static_cast<int>(static_cast<short>(HIWORD(lParam))) };
            ScreenToClient(hwnd, &pt);
            int btn = hitTestButton(pt);
            if (btn == ID_BTN_MINIMIZE || btn == ID_BTN_CLOSE) return HTCLIENT;
            if (pt.y >= 0 && pt.y < sc(56, m_dpi)) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paintWindow(hwnd);
            return 0;
        case WM_DWMCOMPOSITIONCHANGED:
            m_glassBlurActive = enableGlassBackdrop(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE: {
            POINT pt = { static_cast<int>(static_cast<short>(LOWORD(lParam))),
                         static_cast<int>(static_cast<short>(HIWORD(lParam))) };
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);

            size_t cnt = 0;
            {
                std::lock_guard<std::mutex> lock(m_mtx);
                cnt = m_status.receivers.size();
            }
            int newRow = hitTestReceiverRow(pt, cnt);
            int newBtn = (newRow >= 0) ? 0 : hitTestButton(pt);
            if (newRow != m_hoverRowIdx || newBtn != m_hoverBtnId) {
                m_hoverRowIdx = newRow;
                m_hoverBtnId = newBtn;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (m_hoverRowIdx != -1 || m_hoverBtnId != 0) {
                m_hoverRowIdx = -1;
                m_hoverBtnId = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            POINT pt = { static_cast<int>(static_cast<short>(LOWORD(lParam))),
                         static_cast<int>(static_cast<short>(HIWORD(lParam))) };
            int btn = hitTestButton(pt);
            if (btn == ID_IP_BOX) {
                if (!m_ipFocused) {
                    m_ipFocused = true;
                    m_caretOn = true;
                    SetFocus(hwnd);
                    SetTimer(hwnd, TIMER_CARET_BLINK, 530, nullptr);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (m_ipFocused) {
                m_ipFocused = false;
                KillTimer(hwnd, TIMER_CARET_BLINK);
            }
            if (btn != 0) {
                m_pressedBtnId = btn;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            AirPlayTarget chosen;
            bool valid = false;
            {
                std::lock_guard<std::mutex> lock(m_mtx);
                int idx = hitTestReceiverRow(pt, m_status.receivers.size());
                if (idx >= 0 && static_cast<size_t>(idx) < m_status.receivers.size()) {
                    chosen = m_status.receivers[static_cast<size_t>(idx)];
                    valid = true;
                }
            }
            if (valid && m_onSelectTarget) {
                m_onSelectTarget(chosen);
                return 0;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_LBUTTONUP: {
            if (m_pressedBtnId != 0) {
                int releasedOn = hitTestButton({
                    static_cast<int>(static_cast<short>(LOWORD(lParam))),
                    static_cast<int>(static_cast<short>(HIWORD(lParam)))
                });
                int clicked = m_pressedBtnId;
                m_pressedBtnId = 0;
                if (GetCapture() == hwnd) ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                if (releasedOn == clicked) {
                    invokeButtonAction(clicked);
                }
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_CHAR: {
            if (!m_ipFocused) return DefWindowProcW(hwnd, msg, wParam, lParam);
            wchar_t ch = static_cast<wchar_t>(wParam);
            if ((ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L':') {
                if (m_manualIp.size() < 28) {
                    m_manualIp.push_back(static_cast<char>(ch));
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            } else if (ch == L'\b') {
                if (!m_manualIp.empty()) {
                    m_manualIp.pop_back();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            } else if (ch == L'\r') {
                m_ipFocused = false;
                KillTimer(hwnd, TIMER_CARET_BLINK);
                InvalidateRect(hwnd, nullptr, FALSE);
                invokeButtonAction(ID_BTN_CONNECT);
            } else if (ch == 0x1B) {
                m_ipFocused = false;
                KillTimer(hwnd, TIMER_CARET_BLINK);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_KEYDOWN: {
            if (m_ipFocused && wParam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                appendIpCharsFromClipboard();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_KILLFOCUS:
            if (m_ipFocused) {
                m_ipFocused = false;
                KillTimer(hwnd, TIMER_CARET_BLINK);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_TIMER:
            if (wParam == TIMER_CARET_BLINK && m_ipFocused) {
                m_caretOn = !m_caretOn;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_SETCURSOR: {
            if (reinterpret_cast<HWND>(wParam) == hwnd && LOWORD(lParam) == HTCLIENT) {
                POINT pt = {};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                size_t cnt = 0;
                {
                    std::lock_guard<std::mutex> lock(m_mtx);
                    cnt = m_status.receivers.size();
                }
                int btn = hitTestButton(pt);
                if (btn == ID_IP_BOX) {
                    SetCursor(LoadCursorA(nullptr, IDC_IBEAM));
                    return TRUE;
                }
                if (btn != 0 || hitTestReceiverRow(pt, cnt) >= 0) {
                    SetCursor(LoadCursorA(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_APP_REFRESH:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_APP_TRAY: {
            UINT ev = LOWORD(lParam);
            if (ev == WM_LBUTTONDBLCLK || ev == WM_LBUTTONUP) {
                showFromTray();
            } else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
                showTrayContextMenu();
            }
            return 0;
        }
        case WM_DPICHANGED: {
            m_dpi = HIWORD(wParam);
            rebuildFonts();
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) == SC_MINIMIZE) {
                hideToTray();
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                hideToTray();
                return 0;
            }
            return 0;
        case WM_CLOSE:
            removeTrayIcon();
            m_hwnd.store(nullptr);
            if (m_onQuit) m_onQuit();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            removeTrayIcon();
            m_hwnd.store(nullptr);
            KillTimer(hwnd, TIMER_CARET_BLINK);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
