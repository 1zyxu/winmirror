#include "GuiWindow.h"
#include "GlassCanvas.h"
#include "SecProtect.h"
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <thread>

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
constexpr int ID_TRAY_COPY_IP = 2002;
constexpr int ID_TRAY_UPDATE  = 2003;
constexpr int ID_TRAY_QUIT    = 2005;

constexpr int BASE_W = 368;
constexpr int BASE_H = 296;
constexpr int SPLASH_W = 304;
constexpr int SPLASH_H = 84;
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
    bool win11BackdropSet = false;

    std::wstring dwmMod = WM_OBF_W(L"dwmapi.dll");
    if (HMODULE dwm = LoadLibraryW(dwmMod.c_str())) {
        struct DwmMargins { int left, right, top, bottom; };
        using PfnIsComp = HRESULT(WINAPI*)(BOOL*);
        using PfnExtend = HRESULT(WINAPI*)(HWND, const DwmMargins*);
        using PfnSetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

        std::string sIsComp = WM_OBF("DwmIsCompositionEnabled");
        std::string sExtend = WM_OBF("DwmExtendFrameIntoClientArea");
        std::string sSetAttr = WM_OBF("DwmSetWindowAttribute");

        auto pIsComp = reinterpret_cast<PfnIsComp>(GetProcAddress(dwm, sIsComp.c_str()));
        auto pExtend = reinterpret_cast<PfnExtend>(GetProcAddress(dwm, sExtend.c_str()));
        auto pSetAttr = reinterpret_cast<PfnSetAttr>(GetProcAddress(dwm, sSetAttr.c_str()));
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
                DWORD backdrop = 3; // DWMSBT_TRANSIENTWINDOW (Acrylic blur)
                if (SUCCEEDED(pSetAttr(hwnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &backdrop, sizeof(backdrop)))) {
                    win11BackdropSet = true;
                }
            }
        }
        FreeLibrary(dwm);
    }
    wm_sec::wipeWString(dwmMod);

    if (active && !win11BackdropSet) {
        std::wstring u32 = WM_OBF_W(L"user32.dll");
        std::string sCompAttr = WM_OBF("SetWindowCompositionAttribute");
        struct AccentPolicy { int state, flags; DWORD gradColor; int animId; };
        struct WinCompAttr { DWORD attr; PVOID data; ULONG sizeOfData; };
        using PfnSetComp = BOOL(WINAPI*)(HWND, WinCompAttr*);
        auto pSetComp = wm_sec::resolveHiddenApi<PfnSetComp>(u32, sCompAttr);
        wm_sec::wipeWString(u32);
        wm_sec::wipeString(sCompAttr);
        if (pSetComp) {
            AccentPolicy acc = { 4 /* ACCENT_ENABLE_ACRYLICBLURBEHIND */, 2, 0x55221614u, 0 };
            WinCompAttr data = { 19 /* WCA_ACCENT_POLICY */, &acc, sizeof(acc) };
            if (!pSetComp(hwnd, &data)) {
                acc.state = 3; // ACCENT_ENABLE_BLURBEHIND
                acc.gradColor = 0;
                pSetComp(hwnd, &data);
            }
        }
    }
    return active;
}

struct HitRectItem {
    int id;
    RECT rc;
};

RECT receiverRowRect(int idx, UINT dpi) {
    return {
        sc(22, dpi),
        sc(63 + idx * 62, dpi),
        sc(346, dpi),
        sc(119 + idx * 62, dpi)
    };
}

std::vector<HitRectItem> buildControlRects(UINT dpi) {
    return {
        { ID_BTN_MINIMIZE, { sc(302, dpi), sc(15,  dpi), sc(324, dpi), sc(37,  dpi) } },
        { ID_BTN_CLOSE,    { sc(330, dpi), sc(15,  dpi), sc(352, dpi), sc(37,  dpi) } },
        { ID_IP_BOX,       { sc(14,  dpi), sc(200, dpi), sc(206, dpi), sc(232, dpi) } },
        { ID_BTN_CONNECT,  { sc(212, dpi), sc(200, dpi), sc(292, dpi), sc(232, dpi) } },
        { ID_BTN_SCAN,     { sc(298, dpi), sc(200, dpi), sc(354, dpi), sc(232, dpi) } }
    };
}

void drawMirroringGlyph(uint32_t* px, int w, int h, float x, float y, UINT dpi) {
    float s = scf(1.0f, dpi);
    glass::strokeRoundRect(px, w, h, x, y, x + 15 * s, y + 10.5f * s, 2.5f * s, 1.7f * s, {78, 138, 255, 240});
    glass::strokeRoundRect(px, w, h, x + 6 * s, y + 5 * s, x + 21 * s, y + 16 * s, 2.5f * s, 1.7f * s, {146, 114, 255, 245});
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

HICON createAppBadgeIcon() {
    const int sz = 32;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = sz;
    bi.bmiHeader.biHeight = -sz;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screenDc = GetDC(nullptr);
    HBITMAP colorBmp = CreateDIBSection(screenDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP maskBmp = CreateBitmap(sz, sz, 1, 1, nullptr);
    ReleaseDC(nullptr, screenDc);
    if (!colorBmp || !maskBmp || !bits) {
        if (colorBmp) DeleteObject(colorBmp);
        if (maskBmp) DeleteObject(maskBmp);
        return LoadIconA(nullptr, IDI_APPLICATION);
    }

    uint32_t* px = static_cast<uint32_t*>(bits);
    for (int y = 0; y < sz; ++y) {
        for (int x = 0; x < sz; ++x) {
            int dx = std::max(0, std::max(5 - x, x - 26));
            int dy = std::max(0, std::max(5 - y, y - 26));
            if (dx * dx + dy * dy > 25) {
                px[y * sz + x] = 0x00000000;
                continue;
            }
            uint32_t col = (y < 16) ? 0xFF3B82F6 : 0xFF7C3AED;
            if ((x >= 6 && x <= 19 && y >= 7 && y <= 17 && (x == 6 || x == 19 || y == 7 || y == 17)) ||
                (x >= 12 && x <= 25 && y >= 13 && y <= 23 && (x == 12 || x == 25 || y == 13 || y == 23))) {
                col = 0xFFFFFFFF;
            }
            px[y * sz + x] = col;
        }
    }

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
                      const RECT& r, HFONT font, GlassColor col, const std::wstring& txt, UINT fmt) {
    if (!canvasPx || !scratchPx || txt.empty()) return;
    int bw = std::max(1, static_cast<int>(r.right - r.left));
    int bh = std::max(1, static_cast<int>(r.bottom - r.top));
    int boxW = std::min(bw, sw);
    int boxH = std::min(bh, sh);

    for (int y = 0; y < boxH; ++y) {
        std::memset(scratchPx + static_cast<size_t>(y) * sw, 0, static_cast<size_t>(boxW) * 4);
    }

    SetBkMode(scratchDc, TRANSPARENT);
    SetTextColor(scratchDc, RGB(255, 255, 255));
    HGDIOBJ oldF = SelectObject(scratchDc, font);
    RECT localRc = { 0, 0, boxW, boxH };
    DrawTextW(scratchDc, txt.c_str(), -1, &localRc, fmt);
    SelectObject(scratchDc, oldF);

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
            if ((sp & 0x00FFFFFFu) == 0) continue;
            uint32_t sr = (sp >> 16) & 0xFF;
            uint32_t sg = (sp >> 8) & 0xFF;
            uint32_t sb = sp & 0xFF;
            uint32_t cov = (sr + sg * 2 + sb) >> 2;
            if (cov > 0) {
                glass::blendStraight(dRow[dx], col, static_cast<float>(cov) / 255.0f);
            }
        }
    }
}

struct SplashState {
    HWND hwnd = nullptr;
    UINT dpi = 96;
    bool glass = false;
    bool done = false;
    float animPhase = 0.0f;
    HICON icon = nullptr;
    HFONT fTitle = nullptr;
    HFONT fSub = nullptr;
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

            std::string statusCopy;
            {
                std::lock_guard<std::mutex> lock(st->mtx);
                statusCopy = st->statusText;
            }

            glass::fillCanvasGlass(st->canvasPx, w, h, st->glass ? 106 : 255);
            glass::strokeRoundRect(st->canvasPx, w, h, 0.5f, 0.5f, w - 0.5f, h - 0.5f, scf(8, st->dpi), 1.0f, {180, 198, 255, 52});
            drawMirroringGlyph(st->canvasPx, w, h, scf(16, st->dpi), scf(18, st->dpi), st->dpi);
            drawTextToCanvas(st->canvasPx, w, h, st->scratchDc, st->scratchPx, st->sw, st->sh,
                             { sc(46, st->dpi), sc(14, st->dpi), sc(240, st->dpi), sc(34, st->dpi) },
                             st->fTitle, {246, 248, 255, 255}, L"WinMirror",
                             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            drawTextToCanvas(st->canvasPx, w, h, st->scratchDc, st->scratchPx, st->sw, st->sh,
                             { sc(236, st->dpi), sc(14, st->dpi), sc(290, st->dpi), sc(34, st->dpi) },
                             st->fSub, {158, 174, 212, 220}, L"v" L"" WINMIRROR_VERSION,
                             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            drawTextToCanvas(st->canvasPx, w, h, st->scratchDc, st->scratchPx, st->sw, st->sh,
                             { sc(46, st->dpi), sc(35, st->dpi), sc(290, st->dpi), sc(54, st->dpi) },
                             st->fSub, {178, 192, 226, 240}, utf8ToWide(statusCopy),
                             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            float tx0 = scf(16, st->dpi), tx1 = static_cast<float>(w) - scf(16, st->dpi);
            float ty0 = scf(66, st->dpi), ty1 = scf(70, st->dpi);
            glass::fillRoundRectGrad(st->canvasPx, w, h, tx0, ty0, tx1, ty1, scf(2, st->dpi),
                                     {50, 58, 86, 130}, {42, 50, 76, 130}, {0, 0, 0, 0}, 0.0f);
            float span = tx1 - tx0;
            float barW = span * 0.34f;
            float bx0 = tx0 + st->animPhase * (span - barW);
            glass::fillRoundRectGrad(st->canvasPx, w, h, bx0, ty0, bx0 + barW, ty1, scf(2, st->dpi),
                                     {68, 146, 255, 245}, {146, 102, 255, 245}, {0, 0, 0, 0}, 0.0f);

            BitBlt(screenDc, 0, 0, w, h, st->canvasDc, 0, 0, SRCCOPY);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
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

    st.hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        cls.c_str(), L"WinMirror",
        WS_OVERLAPPED | WS_CAPTION,
        CW_USEDEFAULT, CW_USEDEFAULT, SPLASH_W, SPLASH_H,
        nullptr, nullptr, hInst, &st);

    if (!st.hwnd) {
        if (task) task([](const std::string&) {});
        if (st.icon) DestroyIcon(st.icon);
        return;
    }

    st.dpi = queryWindowDpi(st.hwnd);
    st.fTitle = makeUiFont(10, st.dpi, FW_SEMIBOLD);
    st.fSub = makeUiFont(8, st.dpi, FW_NORMAL);
    st.glass = enableGlassBackdrop(st.hwnd);

    int w = sc(SPLASH_W, st.dpi);
    int h = sc(SPLASH_H, st.dpi);
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int posX = work.left + ((work.right - work.left) - w) / 2;
    int posY = work.top + ((work.bottom - work.top) - h) / 2;
    SetWindowPos(st.hwnd, HWND_TOPMOST, posX, posY, w, h, SWP_FRAMECHANGED);

    ShowWindow(st.hwnd, SW_SHOW);
    UpdateWindow(st.hwnd);
    SetTimer(st.hwnd, TIMER_SPLASH_ANIM, 30, nullptr);

    std::thread worker([&]() {
        wm_sec::hideCurrentThread();
        if (task) {
            task([&](const std::string& msg) {
                {
                    std::lock_guard<std::mutex> lock(st.mtx);
                    st.statusText = msg;
                }
                if (st.hwnd) PostMessageW(st.hwnd, WM_APP_SPLASH_STATUS, 0, 0);
            });
        }
        if (st.hwnd) PostMessageW(st.hwnd, WM_APP_SPLASH_DONE, 0, 0);
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
    if (m_appIcon) { DestroyIcon(m_appIcon); m_appIcon = nullptr; }
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
    drawTextToCanvas(m_canvasPx, m_canvasW, m_canvasH,
                     m_scratchDc, m_scratchPx, m_scratchW, m_scratchH,
                     r, font, col, txt, fmt);
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
    m_fontLabel = makeUiFont(7,  m_dpi, FW_SEMIBOLD);
    m_fontBody  = makeUiFont(8,  m_dpi, FW_NORMAL);
    m_fontSmall = makeUiFont(8,  m_dpi, FW_NORMAL);
}

bool WinMirrorGui::createAndShow() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    std::wstring clsName = WM_OBF_W(L"WMAcrylicMirrorCard");

    m_appIcon = createAppBadgeIcon();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProcThunk;
    wc.hInstance = hInst;
    wc.hIcon = m_appIcon;
    wc.hIconSm = m_appIcon;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = clsName.c_str();
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const DWORD exStyle = WS_EX_APPWINDOW;

    m_hwnd = CreateWindowExW(
        exStyle, clsName.c_str(), L"Screen Mirroring", style,
        CW_USEDEFAULT, CW_USEDEFAULT, BASE_W, BASE_H,
        nullptr, nullptr, hInst, this);
    wm_sec::wipeWString(clsName);
    if (!m_hwnd) return false;

    m_dpi = queryWindowDpi(m_hwnd);
    rebuildFonts();
    m_glassBlurActive = enableGlassBackdrop(m_hwnd);

    int winW = sc(BASE_W, m_dpi);
    int winH = sc(BASE_H, m_dpi);
    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int posX = work.left + ((work.right - work.left) - winW) / 2;
    int posY = work.top + ((work.bottom - work.top) - winH) / 2;
    SetWindowPos(m_hwnd, nullptr, posX, posY, winW, winH, SWP_NOZORDER | SWP_FRAMECHANGED);

    addTrayIcon();
    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    return true;
}

void WinMirrorGui::updateStatus(const WinMirrorStatus& st) {
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_status = st;
    }
    if (m_hwnd) {
        PostMessageW(m_hwnd, WM_APP_REFRESH, 0, 0);
    }
}

void WinMirrorGui::setStatusBanner(const std::string& msg) {
    if (msg.empty()) return;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_status.statusBanner = msg;
    }
    if (m_hwnd) {
        PostMessageW(m_hwnd, WM_APP_REFRESH, 0, 0);
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
    if (m_hwnd) {
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
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
        ShowWindow(m_hwnd, SW_MINIMIZE);
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
    nid.hIcon = m_appIcon;
    std::wcsncpy(nid.szTip, L"Screen Mirroring", ARRAYSIZE(nid.szTip) - 1);
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
    AppendMenuW(menu, MF_STRING, ID_TRAY_COPY_IP, L"Copy PC AirPlay IP");
    AppendMenuW(menu, MF_STRING, ID_TRAY_UPDATE, L"Check for Updates (v" L"" WINMIRROR_VERSION L")");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT, L"Quit");

    SetForegroundWindow(m_hwnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                              pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == ID_TRAY_OPEN) {
        showFromTray();
    } else if (cmd == ID_TRAY_COPY_IP) {
        copyPrimaryAddress();
    } else if (cmd == ID_TRAY_UPDATE) {
        if (m_onCheckUpdate) m_onCheckUpdate();
    } else if (cmd == ID_TRAY_QUIT) {
        requestClose();
    }
}

void WinMirrorGui::copyPrimaryAddress() {
    std::string addr;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        addr = m_status.primaryConnectAddr;
    }
    if (addr.empty()) addr = "127.0.0.1:7001";
    std::wstring waddr = utf8ToWide(addr);
    if (OpenClipboard(m_hwnd)) {
        EmptyClipboard();
        size_t bytes = (waddr.size() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hMem) {
            void* p = GlobalLock(hMem);
            if (p) {
                memcpy(p, waddr.c_str(), bytes);
                GlobalUnlock(hMem);
                SetClipboardData(CF_UNICODETEXT, hMem);
            } else {
                GlobalFree(hMem);
            }
        }
        CloseClipboard();
    }
    setStatusBanner("Copied PC AirPlay address: " + addr);
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

    WinMirrorStatus snap;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        snap = m_status;
    }

    glass::fillCanvasGlass(m_canvasPx, cw, ch, m_glassBlurActive ? 102 : 255);
    glass::strokeRoundRect(m_canvasPx, cw, ch, 0.5f, 0.5f, cw - 0.5f, ch - 0.5f,
                           scf(8, m_dpi), 1.0f, {178, 196, 255, 48});

    const GlassColor cWhite = {246, 248, 255, 255};
    const GlassColor cMuted = {166, 178, 208, 230};
    const GlassColor cCyan  = {88, 212, 255, 255};
    const GlassColor cGreen = {64, 224, 158, 255};
    const GlassColor cAmber = {252, 200, 72, 255};

    // Header: Glowing Mirroring Icon + Title + Compact Live Pill + Window Chrome
    drawMirroringGlyph(m_canvasPx, cw, ch, scf(16, m_dpi), scf(17, m_dpi), m_dpi);
    drawTextGlass({ sc(45, m_dpi), sc(11, m_dpi), sc(218, m_dpi), sc(31, m_dpi) },
                  m_fontTitle, cWhite, L"Screen Mirroring",
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    drawTextGlass({ sc(45, m_dpi), sc(29, m_dpi), sc(218, m_dpi), sc(44, m_dpi) },
                  m_fontLabel, cMuted, L"AirPlay Zero-Latency",
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    glass::fillRoundRectGrad(m_canvasPx, cw, ch,
                             scf(222, m_dpi), scf(15, m_dpi), scf(295, m_dpi), scf(37, m_dpi),
                             scf(11, m_dpi),
                             snap.clients > 0 ? GlassColor{24, 68, 56, 170} : GlassColor{34, 40, 62, 150},
                             snap.clients > 0 ? GlassColor{18, 52, 46, 170} : GlassColor{26, 32, 52, 150},
                             snap.clients > 0 ? GlassColor{68, 210, 150, 190} : GlassColor{150, 168, 220, 95}, 1.0f);
    glass::fillAaCircle(m_canvasPx, cw, ch, scf(233, m_dpi), scf(26, m_dpi), scf(3.3f, m_dpi),
                        snap.clients > 0 ? cGreen : (snap.streamingReady ? cCyan : cAmber));
    wchar_t pillTxt[32] = {};
    if (snap.clients > 0) {
        std::swprintf(pillTxt, ARRAYSIZE(pillTxt), L"Live \u2022 %.0f", snap.actualFps);
    } else if (snap.streamingReady) {
        std::swprintf(pillTxt, ARRAYSIZE(pillTxt), L"Ready");
    } else {
        std::swprintf(pillTxt, ARRAYSIZE(pillTxt), L"Init...");
    }
    drawTextGlass({ sc(241, m_dpi), sc(15, m_dpi), sc(291, m_dpi), sc(37, m_dpi) },
                  m_fontLabel, cWhite, pillTxt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const auto ctrls = buildControlRects(m_dpi);
    auto rectOf = [&](int id) -> RECT {
        for (const auto& c : ctrls) if (c.id == id) return c.rc;
        return {};
    };

    // Custom Minimal Chrome Buttons (- and X)
    RECT rcMin = rectOf(ID_BTN_MINIMIZE);
    bool minHv = (m_hoverBtnId == ID_BTN_MINIMIZE);
    glass::fillRoundRectGrad(m_canvasPx, cw, ch,
                             static_cast<float>(rcMin.left), static_cast<float>(rcMin.top),
                             static_cast<float>(rcMin.right), static_cast<float>(rcMin.bottom),
                             scf(7, m_dpi),
                             minHv ? GlassColor{72, 80, 114, 175} : GlassColor{38, 44, 68, 120},
                             minHv ? GlassColor{58, 66, 98, 175} : GlassColor{30, 36, 58, 120},
                             {156, 174, 224, minHv ? uint8_t(150) : uint8_t(72)}, 1.0f);
    glass::drawAaSegment(m_canvasPx, cw, ch,
                         static_cast<float>(rcMin.left + sc(7, m_dpi)),
                         static_cast<float>((rcMin.top + rcMin.bottom) / 2),
                         static_cast<float>(rcMin.right - sc(7, m_dpi)),
                         static_cast<float>((rcMin.top + rcMin.bottom) / 2),
                         scf(1.4f, m_dpi), cWhite);

    RECT rcClose = rectOf(ID_BTN_CLOSE);
    bool closeHv = (m_hoverBtnId == ID_BTN_CLOSE);
    glass::fillRoundRectGrad(m_canvasPx, cw, ch,
                             static_cast<float>(rcClose.left), static_cast<float>(rcClose.top),
                             static_cast<float>(rcClose.right), static_cast<float>(rcClose.bottom),
                             scf(7, m_dpi),
                             closeHv ? GlassColor{216, 62, 74, 215} : GlassColor{38, 44, 68, 120},
                             closeHv ? GlassColor{184, 48, 60, 215} : GlassColor{30, 36, 58, 120},
                             closeHv ? GlassColor{255, 136, 146, 200} : GlassColor{156, 174, 224, 72}, 1.0f);
    glass::drawAaSegment(m_canvasPx, cw, ch,
                         static_cast<float>(rcClose.left + sc(7, m_dpi)),
                         static_cast<float>(rcClose.top + sc(7, m_dpi)),
                         static_cast<float>(rcClose.right - sc(7, m_dpi)),
                         static_cast<float>(rcClose.bottom - sc(7, m_dpi)),
                         scf(1.4f, m_dpi), cWhite);
    glass::drawAaSegment(m_canvasPx, cw, ch,
                         static_cast<float>(rcClose.right - sc(7, m_dpi)),
                         static_cast<float>(rcClose.top + sc(7, m_dpi)),
                         static_cast<float>(rcClose.left + sc(7, m_dpi)),
                         static_cast<float>(rcClose.bottom - sc(7, m_dpi)),
                         scf(1.4f, m_dpi), cWhite);

    // Compact Frosted Receiver Card Container
    glass::fillRoundRectGrad(m_canvasPx, cw, ch,
                             scf(14, m_dpi), scf(54, m_dpi), scf(354, m_dpi), scf(190, m_dpi),
                             scf(14, m_dpi),
                             {34, 38, 60, 124}, {22, 26, 44, 136},
                             {170, 188, 232, 92}, 1.0f);

    if (snap.receivers.empty()) {
        drawLaptopGlyph(m_canvasPx, cw, ch, scf(172, m_dpi), scf(88, m_dpi), m_dpi, cMuted);
        drawTextGlass({ sc(24, m_dpi), sc(112, m_dpi), sc(344, m_dpi), sc(134, m_dpi) },
                      m_fontRow, cWhite, L"Looking for AirPlay devices...",
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        drawTextGlass({ sc(24, m_dpi), sc(134, m_dpi), sc(344, m_dpi), sc(154, m_dpi) },
                      m_fontSmall, cMuted, L"Open OBS on Mac or enter IP below",
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
                    hovered ? GlassColor{96, 94, 212, 188} : GlassColor{84, 82, 196, 178},
                    hovered ? GlassColor{62, 84, 176, 182} : GlassColor{52, 72, 152, 172},
                    {136, 174, 255, 220}, 1.15f);
            } else {
                glass::fillRoundRectGrad(
                    m_canvasPx, cw, ch,
                    static_cast<float>(row.left), static_cast<float>(row.top),
                    static_cast<float>(row.right), static_cast<float>(row.bottom),
                    scf(11, m_dpi),
                    hovered ? GlassColor{68, 76, 112, 160} : GlassColor{38, 44, 70, 98},
                    hovered ? GlassColor{52, 60, 96, 160} : GlassColor{28, 34, 58, 98},
                    hovered ? GlassColor{156, 176, 232, 160} : GlassColor{132, 148, 192, 64}, 1.0f);
            }

            GlassColor iconCol = dev.isConnected ? cCyan : (dev.isDirectObs ? cWhite : cMuted);
            if (dev.isLaptop) {
                drawLaptopGlyph(m_canvasPx, cw, ch,
                                static_cast<float>(row.left + sc(13, m_dpi)),
                                static_cast<float>(row.top + sc(19, m_dpi)), m_dpi, iconCol);
            } else {
                drawTvGlyph(m_canvasPx, cw, ch,
                            static_cast<float>(row.left + sc(13, m_dpi)),
                            static_cast<float>(row.top + sc(19, m_dpi)), m_dpi, iconCol);
            }

            drawTextGlass({ row.left + sc(46, m_dpi), row.top + sc(7, m_dpi),
                            row.right - sc(86, m_dpi), row.top + sc(28, m_dpi) },
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
                std::swprintf(subLine, ARRAYSIZE(subLine), L"OBS \u2022 %hs", dev.ip.c_str());
            } else {
                std::swprintf(subLine, ARRAYSIZE(subLine), L"AirPlay \u2022 %hs", dev.ip.c_str());
            }
            drawTextGlass({ row.left + sc(46, m_dpi), row.top + sc(29, m_dpi),
                            row.right - sc(86, m_dpi), row.top + sc(47, m_dpi) },
                          m_fontSmall, cMuted, subLine,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            if (dev.isConnected) {
                float bx0 = static_cast<float>(row.right - sc(78, m_dpi));
                float by0 = static_cast<float>(row.top + sc(15, m_dpi));
                float bx1 = static_cast<float>(row.right - sc(10, m_dpi));
                float by1 = static_cast<float>(row.bottom - sc(15, m_dpi));
                glass::fillRoundRectGrad(m_canvasPx, cw, ch, bx0, by0, bx1, by1, scf(12, m_dpi),
                                         {34, 128, 92, 228}, {24, 102, 74, 228}, {76, 222, 160, 215}, 1.0f);
                drawCheckGlyph(m_canvasPx, cw, ch, bx0 + scf(9, m_dpi), by0 + scf(9, m_dpi), m_dpi, cWhite);
                drawTextGlass({ static_cast<LONG>(bx0) + sc(21, m_dpi), static_cast<LONG>(by0),
                                static_cast<LONG>(bx1) - sc(6, m_dpi), static_cast<LONG>(by1) },
                              m_fontLabel, cWhite, L"Active", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            } else if (dev.isDirectObs) {
                float bx0 = static_cast<float>(row.right - sc(54, m_dpi));
                float by0 = static_cast<float>(row.top + sc(17, m_dpi));
                float bx1 = static_cast<float>(row.right - sc(10, m_dpi));
                float by1 = static_cast<float>(row.bottom - sc(17, m_dpi));
                glass::fillRoundRectGrad(m_canvasPx, cw, ch, bx0, by0, bx1, by1, scf(9, m_dpi),
                                         {52, 82, 160, 175}, {44, 68, 138, 175}, {122, 162, 255, 165}, 1.0f);
                drawTextGlass({ static_cast<LONG>(bx0), static_cast<LONG>(by0),
                                static_cast<LONG>(bx1), static_cast<LONG>(by1) },
                              m_fontLabel, cWhite, L"OBS", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                drawTextGlass({ row.right - sc(64, m_dpi), row.top, row.right - sc(10, m_dpi), row.bottom },
                              m_fontLabel, cMuted, L"AirPlay", DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
        }
    }

    // 1) Compact IP Pill
    RECT rcIp = rectOf(ID_IP_BOX);
    glass::fillRoundRectGrad(m_canvasPx, cw, ch,
                             static_cast<float>(rcIp.left), static_cast<float>(rcIp.top),
                             static_cast<float>(rcIp.right), static_cast<float>(rcIp.bottom),
                             scf(10, m_dpi),
                             {30, 34, 54, 152}, {24, 28, 46, 152},
                             m_ipFocused ? GlassColor{96, 182, 255, 225}
                                         : (m_hoverBtnId == ID_IP_BOX ? GlassColor{156, 174, 226, 145}
                                                                      : GlassColor{136, 152, 198, 85}),
                             m_ipFocused ? 1.3f : 1.0f);
    drawMonitorGlyph(m_canvasPx, cw, ch,
                     static_cast<float>(rcIp.left + sc(10, m_dpi)),
                     static_cast<float>(rcIp.top + sc(10, m_dpi)), m_dpi,
                     m_ipFocused ? cCyan : cMuted);

    std::wstring ipLabel;
    GlassColor ipTextCol = cWhite;
    if (!m_manualIp.empty()) {
        ipLabel = utf8ToWide(m_manualIp);
        if (m_ipFocused && m_caretOn) ipLabel += L"|";
    } else if (m_ipFocused) {
        ipLabel = m_caretOn ? L"| Type Mac IP..." : L"  Type Mac IP...";
        ipTextCol = cMuted;
    } else {
        std::string autoIp;
        for (const auto& t : snap.receivers) {
            if (t.isDirectObs) { autoIp = t.ip; break; }
        }
        ipLabel = !autoIp.empty() ? (L"Mac: " + utf8ToWide(autoIp)) : L"Enter Mac IP...";
        ipTextCol = cMuted;
    }
    drawTextGlass({ rcIp.left + sc(29, m_dpi), rcIp.top, rcIp.right - sc(8, m_dpi), rcIp.bottom },
                  m_fontBody, ipTextCol, ipLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // 2) Gradient Connect Pill
    RECT rcConn = rectOf(ID_BTN_CONNECT);
    bool connHv = (m_hoverBtnId == ID_BTN_CONNECT);
    bool connDn = (m_pressedBtnId == ID_BTN_CONNECT);
    glass::fillRoundRectGrad(m_canvasPx, cw, ch,
                             static_cast<float>(rcConn.left), static_cast<float>(rcConn.top),
                             static_cast<float>(rcConn.right), static_cast<float>(rcConn.bottom),
                             scf(10, m_dpi),
                             connDn ? GlassColor{48, 96, 212, 245} : (connHv ? GlassColor{78, 146, 255, 250} : GlassColor{60, 128, 252, 245}),
                             connDn ? GlassColor{104, 62, 198, 245} : (connHv ? GlassColor{154, 102, 255, 250} : GlassColor{134, 82, 244, 245}),
                             {178, 198, 255, 190}, 1.0f);
    glass::fillAaPlayTriangle(m_canvasPx, cw, ch,
                              static_cast<float>(rcConn.left + sc(16, m_dpi)),
                              static_cast<float>((rcConn.top + rcConn.bottom) / 2),
                              scf(8.5f, m_dpi), cWhite);
    drawTextGlass({ rcConn.left + sc(25, m_dpi), rcConn.top, rcConn.right - sc(6, m_dpi), rcConn.bottom },
                  m_fontBody, cWhite, L"Connect", DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 3) Compact Scan Pill
    RECT rcScan = rectOf(ID_BTN_SCAN);
    bool scanHv = (m_hoverBtnId == ID_BTN_SCAN);
    glass::fillRoundRectGrad(m_canvasPx, cw, ch,
                             static_cast<float>(rcScan.left), static_cast<float>(rcScan.top),
                             static_cast<float>(rcScan.right), static_cast<float>(rcScan.bottom),
                             scf(10, m_dpi),
                             scanHv ? GlassColor{58, 66, 98, 185} : GlassColor{38, 44, 68, 160},
                             scanHv ? GlassColor{44, 52, 82, 185} : GlassColor{28, 34, 56, 160},
                             {156, 174, 224, scanHv ? uint8_t(165) : uint8_t(95)}, 1.0f);
    drawRefreshGlyph(m_canvasPx, cw, ch,
                     static_cast<float>(rcScan.left + sc(14, m_dpi)),
                     static_cast<float>((rcScan.top + rcScan.bottom) / 2), m_dpi, cWhite);
    drawTextGlass({ rcScan.left + sc(22, m_dpi), rcScan.top, rcScan.right - sc(4, m_dpi), rcScan.bottom },
                  m_fontBody, cWhite, L"Scan", DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Compact Telemetry Spec Strip
    int specY0 = sc(241, m_dpi), specY1 = sc(260, m_dpi);
    drawChipGlyph(m_canvasPx, cw, ch, scf(16, m_dpi), scf(245, m_dpi), m_dpi, cMuted);
    drawTextGlass({ sc(31, m_dpi), specY0, sc(80, m_dpi), specY1 },
                  m_fontSmall, cWhite, snap.isHevc ? L"HEVC" : L"H.264", DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    glass::drawAaSegment(m_canvasPx, cw, ch, scf(85, m_dpi), scf(244, m_dpi), scf(85, m_dpi), scf(256, m_dpi), 1.0f, {150, 165, 210, 75});
    drawGaugeGlyph(m_canvasPx, cw, ch, scf(98, m_dpi), scf(250, m_dpi), m_dpi, cMuted);
    wchar_t fpsSpec[32] = {};
    std::swprintf(fpsSpec, ARRAYSIZE(fpsSpec), L"%.0f FPS", snap.clients > 0 ? snap.actualFps : static_cast<float>(snap.targetFps));
    drawTextGlass({ sc(109, m_dpi), specY0, sc(162, m_dpi), specY1 },
                  m_fontSmall, cWhite, fpsSpec, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    glass::drawAaSegment(m_canvasPx, cw, ch, scf(166, m_dpi), scf(244, m_dpi), scf(166, m_dpi), scf(256, m_dpi), 1.0f, {150, 165, 210, 75});
    glass::fillAaCircle(m_canvasPx, cw, ch, scf(178, m_dpi), scf(250, m_dpi), scf(2.8f, m_dpi), snap.clients > 0 ? cCyan : cMuted);
    wchar_t mbpsSpec[40] = {};
    if (snap.clients > 0) {
        std::swprintf(mbpsSpec, ARRAYSIZE(mbpsSpec), L"%.1f Mbps", snap.actualMbps);
    } else {
        std::swprintf(mbpsSpec, ARRAYSIZE(mbpsSpec), L"%u Mbps", snap.targetMbps ? snap.targetMbps : 200);
    }
    drawTextGlass({ sc(187, m_dpi), specY0, sc(262, m_dpi), specY1 },
                  m_fontSmall, cWhite, mbpsSpec, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    glass::drawAaSegment(m_canvasPx, cw, ch, scf(267, m_dpi), scf(244, m_dpi), scf(267, m_dpi), scf(256, m_dpi), 1.0f, {150, 165, 210, 75});
    drawSpeakerGlyph(m_canvasPx, cw, ch, scf(279, m_dpi), scf(245, m_dpi), m_dpi, snap.audioActive ? cWhite : cMuted);
    drawTextGlass({ sc(294, m_dpi), specY0, sc(354, m_dpi), specY1 },
                  m_fontSmall, snap.audioActive ? cWhite : cMuted,
                  snap.audioActive ? L"Audio" : L"Muted", DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Compact Live Status Line
    glass::fillAaCircle(m_canvasPx, cw, ch, scf(20, m_dpi), scf(276, m_dpi), scf(3.6f, m_dpi),
                        snap.clients > 0 ? cGreen : cCyan);
    drawTextGlass({ sc(30, m_dpi), sc(266, m_dpi), sc(354, m_dpi), sc(286, m_dpi) },
                  m_fontLabel, snap.clients > 0 ? cWhite : cMuted,
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
        case WM_NCHITTEST: {
            POINT pt = { static_cast<int>(static_cast<short>(LOWORD(lParam))),
                         static_cast<int>(static_cast<short>(HIWORD(lParam))) };
            ScreenToClient(hwnd, &pt);
            int btn = hitTestButton(pt);
            if (btn == ID_BTN_MINIMIZE || btn == ID_BTN_CLOSE) return HTCLIENT;
            if (pt.y >= 0 && pt.y < sc(50, m_dpi)) return HTCAPTION;
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
        case WM_CLOSE:
            removeTrayIcon();
            if (m_onQuit) m_onQuit();
            DestroyWindow(hwnd);
            m_hwnd = nullptr;
            return 0;
        case WM_DESTROY:
            removeTrayIcon();
            KillTimer(hwnd, TIMER_CARET_BLINK);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
