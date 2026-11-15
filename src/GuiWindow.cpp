#include "GuiWindow.h"
#include "GlassCanvas.h"
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {

constexpr UINT WM_APP_REFRESH = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;
constexpr UINT_PTR TIMER_CARET_BLINK = 102;

constexpr int ID_IP_BOX      = 1000;
constexpr int ID_BTN_CONNECT = 1001;
constexpr int ID_BTN_SCAN    = 1002;

constexpr int ID_TRAY_OPEN    = 2001;
constexpr int ID_TRAY_COPY_IP = 2002;
constexpr int ID_TRAY_UPDATE  = 2003;
constexpr int ID_TRAY_QUIT    = 2005;

constexpr int BASE_W = 442;
constexpr int BASE_H = 404;
constexpr size_t MAX_VISIBLE_ROWS = 3;

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
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        using PfnGetDpiForWindow = UINT(WINAPI*)(HWND);
        auto fn = reinterpret_cast<PfnGetDpiForWindow>(GetProcAddress(u32, "GetDpiForWindow"));
        if (fn && hwnd) {
            UINT d = fn(hwnd);
            if (d >= 72) return d;
        }
    }
    return 96;
}

bool enableGlassBackdrop(HWND hwnd) {
    bool active = false;
    bool win11BackdropSet = false;

    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        struct DwmMargins { int left, right, top, bottom; };
        using PfnIsComp = HRESULT(WINAPI*)(BOOL*);
        using PfnExtend = HRESULT(WINAPI*)(HWND, const DwmMargins*);
        using PfnSetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

        auto pIsComp = reinterpret_cast<PfnIsComp>(GetProcAddress(dwm, "DwmIsCompositionEnabled"));
        auto pExtend = reinterpret_cast<PfnExtend>(GetProcAddress(dwm, "DwmExtendFrameIntoClientArea"));
        auto pSetAttr = reinterpret_cast<PfnSetAttr>(GetProcAddress(dwm, "DwmSetWindowAttribute"));

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

    if (active && !win11BackdropSet) {
        // Windows 10 fallback via SetWindowCompositionAttribute
        if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
            struct AccentPolicy { int state, flags; DWORD gradColor; int animId; };
            struct WinCompAttr { DWORD attr; PVOID data; ULONG sizeOfData; };
            using PfnSetComp = BOOL(WINAPI*)(HWND, WinCompAttr*);
            auto pSetComp = reinterpret_cast<PfnSetComp>(GetProcAddress(u32, "SetWindowCompositionAttribute"));
            if (pSetComp) {
                AccentPolicy acc = { 4 /* ACCENT_ENABLE_ACRYLICBLURBEHIND */, 2, 0x55121424u, 0 };
                WinCompAttr data = { 19 /* WCA_ACCENT_POLICY */, &acc, sizeof(acc) };
                if (!pSetComp(hwnd, &data)) {
                    acc.state = 3; // ACCENT_ENABLE_BLURBEHIND
                    acc.gradColor = 0;
                    pSetComp(hwnd, &data);
                }
            }
        }
    }
    return active;
}

void adjustRectForWindowDpi(RECT& rc, DWORD style, DWORD exStyle, UINT dpi) {
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        using PfnAdj = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
        auto fn = reinterpret_cast<PfnAdj>(GetProcAddress(u32, "AdjustWindowRectExForDpi"));
        if (fn && fn(&rc, style, FALSE, exStyle, dpi)) return;
    }
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
}

struct HitRectItem {
    int id;
    RECT rc;
};

RECT receiverRowRect(int idx, UINT dpi) {
    return {
        sc(28, dpi),
        sc(78 + idx * 60, dpi),
        sc(414, dpi),
        sc(132 + idx * 60, dpi)
    };
}

std::vector<HitRectItem> buildControlRects(UINT dpi) {
    return {
        { ID_IP_BOX,      { sc(18,  dpi), sc(278, dpi), sc(244, dpi), sc(314, dpi) } },
        { ID_BTN_CONNECT, { sc(252, dpi), sc(278, dpi), sc(342, dpi), sc(314, dpi) } },
        { ID_BTN_SCAN,    { sc(350, dpi), sc(278, dpi), sc(424, dpi), sc(314, dpi) } }
    };
}

void drawMirroringGlyph(uint32_t* px, int w, int h, float x, float y, UINT dpi) {
    float s = scf(1.0f, dpi);
    glass::strokeRoundRect(px, w, h, x, y, x + 17 * s, y + 12 * s, 3 * s, 1.8f * s, {78, 138, 255, 240});
    glass::strokeRoundRect(px, w, h, x + 7 * s, y + 6 * s, x + 25 * s, y + 19 * s, 3 * s, 1.8f * s, {146, 114, 255, 245});
}

void drawLaptopGlyph(uint32_t* px, int w, int h, float x, float y, UINT dpi, GlassColor col) {
    float s = scf(1.0f, dpi);
    glass::strokeRoundRect(px, w, h, x + 3 * s, y + 1 * s, x + 23 * s, y + 14 * s, 2.5f * s, 1.7f * s, col);
    glass::drawAaSegment(px, w, h, x, y + 17 * s, x + 26 * s, y + 17 * s, 1.8f * s, col);
}

void drawTvGlyph(uint32_t* px, int w, int h, float x, float y, UINT dpi, GlassColor col) {
    float s = scf(1.0f, dpi);
    glass::strokeRoundRect(px, w, h, x + 2 * s, y, x + 24 * s, y + 15 * s, 2.5f * s, 1.7f * s, col);
    glass::drawAaSegment(px, w, h, x + 8 * s, y + 18 * s, x + 18 * s, y + 18 * s, 1.8f * s, col);
}

void drawMonitorGlyph(uint32_t* px, int w, int h, float x, float y, UINT dpi, GlassColor col) {
    float s = scf(1.0f, dpi);
    glass::strokeRoundRect(px, w, h, x, y, x + 14 * s, y + 10 * s, 2 * s, 1.4f * s, col);
    glass::drawAaSegment(px, w, h, x + 4 * s, y + 12.5f * s, x + 10 * s, y + 12.5f * s, 1.5f * s, col);
}

void drawCheckGlyph(uint32_t* px, int w, int h, float x, float y, UINT dpi, GlassColor col) {
    float s = scf(1.0f, dpi);
    glass::drawAaSegment(px, w, h, x, y + 4.5f * s, x + 3.5f * s, y + 8 * s, 1.8f * s, col);
    glass::drawAaSegment(px, w, h, x + 3.5f * s, y + 8 * s, x + 10 * s, y + 1 * s, 1.8f * s, col);
}

void drawRefreshGlyph(uint32_t* px, int w, int h, float cx, float cy, UINT dpi, GlassColor col) {
    float s = scf(1.0f, dpi);
    glass::drawAaCircleRing(px, w, h, cx, cy, 5.2f * s, 1.4f * s, col);
    glass::drawAaSegment(px, w, h, cx + 2.5f * s, cy - 5.5f * s, cx + 6 * s, cy - 2.5f * s, 1.4f * s, col);
}

void drawGaugeGlyph(uint32_t* px, int w, int h, float cx, float cy, UINT dpi, GlassColor col) {
    float s = scf(1.0f, dpi);
    glass::drawAaCircleRing(px, w, h, cx, cy, 5.5f * s, 1.3f * s, col);
    glass::drawAaSegment(px, w, h, cx, cy, cx + 3.2f * s, cy - 2.8f * s, 1.4f * s, col);
}

void drawChipGlyph(uint32_t* px, int w, int h, float x, float y, UINT dpi, GlassColor col) {
    float s = scf(1.0f, dpi);
    glass::strokeRoundRect(px, w, h, x + 2 * s, y + 2 * s, x + 10 * s, y + 10 * s, 1.5f * s, 1.2f * s, col);
    glass::drawAaSegment(px, w, h, x, y + 4 * s, x + 2 * s, y + 4 * s, 1.2f * s, col);
    glass::drawAaSegment(px, w, h, x, y + 8 * s, x + 2 * s, y + 8 * s, 1.2f * s, col);
    glass::drawAaSegment(px, w, h, x + 10 * s, y + 4 * s, x + 12 * s, y + 4 * s, 1.2f * s, col);
    glass::drawAaSegment(px, w, h, x + 10 * s, y + 8 * s, x + 12 * s, y + 8 * s, 1.2f * s, col);
}

void drawSpeakerGlyph(uint32_t* px, int w, int h, float x, float y, UINT dpi, GlassColor col) {
    float s = scf(1.0f, dpi);
    glass::strokeRoundRect(px, w, h, x, y + 3 * s, x + 4 * s, y + 9 * s, 1 * s, 1.3f * s, col);
    glass::drawAaSegment(px, w, h, x + 4 * s, y + 3 * s, x + 8 * s, y + 0.5f * s, 1.4f * s, col);
    glass::drawAaSegment(px, w, h, x + 8 * s, y + 0.5f * s, x + 8 * s, y + 11.5f * s, 1.4f * s, col);
    glass::drawAaSegment(px, w, h, x + 8 * s, y + 11.5f * s, x + 4 * s, y + 9 * s, 1.4f * s, col);
    glass::drawAaSegment(px, w, h, x + 11 * s, y + 3.5f * s, x + 11 * s, y + 8.5f * s, 1.4f * s, col);
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

} // namespace

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
    if (!m_canvasPx || !m_scratchPx || txt.empty()) return;
    int bw = std::max(1, static_cast<int>(r.right - r.left));
    int bh = std::max(1, static_cast<int>(r.bottom - r.top));
    int boxW = std::min(bw, m_scratchW);
    int boxH = std::min(bh, m_scratchH);

    for (int y = 0; y < boxH; ++y) {
        std::memset(m_scratchPx + static_cast<size_t>(y) * m_scratchW, 0, static_cast<size_t>(boxW) * 4);
    }

    SetBkMode(m_scratchDc, TRANSPARENT);
    SetTextColor(m_scratchDc, RGB(255, 255, 255));
    HGDIOBJ oldF = SelectObject(m_scratchDc, font);
    RECT localRc = { 0, 0, boxW, boxH };
    DrawTextW(m_scratchDc, txt.c_str(), -1, &localRc, fmt);
    SelectObject(m_scratchDc, oldF);

    int dstX0 = std::max<int>(0, r.left);
    int dstY0 = std::max<int>(0, r.top);
    int dstX1 = std::min<int>(m_canvasW, r.left + boxW);
    int dstY1 = std::min<int>(m_canvasH, r.top + boxH);
    for (int dy = dstY0; dy < dstY1; ++dy) {
        int sy = dy - r.top;
        const uint32_t* sRow = m_scratchPx + static_cast<size_t>(sy) * m_scratchW;
        uint32_t* dRow = m_canvasPx + static_cast<size_t>(dy) * m_canvasW;
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

    m_fontTitle = makeUiFont(14, m_dpi, FW_SEMIBOLD);
    m_fontRow   = makeUiFont(11, m_dpi, FW_SEMIBOLD);
    m_fontLabel = makeUiFont(8,  m_dpi, FW_SEMIBOLD);
    m_fontBody  = makeUiFont(9,  m_dpi, FW_NORMAL);
    m_fontSmall = makeUiFont(8,  m_dpi, FW_NORMAL);
}

bool WinMirrorGui::createAndShow() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    const wchar_t* clsName = L"WinMirrorAcrylicGlassWnd";

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
    wc.lpszClassName = clsName;
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const DWORD exStyle = WS_EX_APPWINDOW;

    m_hwnd = CreateWindowExW(
        exStyle, clsName, L"Screen Mirroring", style,
        CW_USEDEFAULT, CW_USEDEFAULT, 456, 436,
        nullptr, nullptr, hInst, this);
    if (!m_hwnd) return false;

    m_dpi = queryWindowDpi(m_hwnd);
    rebuildFonts();
    m_glassBlurActive = enableGlassBackdrop(m_hwnd);

    RECT rc = { 0, 0, sc(BASE_W, m_dpi), sc(BASE_H, m_dpi) };
    adjustRectForWindowDpi(rc, style, exStyle, m_dpi);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;

    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int posX = work.left + ((work.right - work.left) - winW) / 2;
    int posY = work.top + ((work.bottom - work.top) - winH) / 2;
    SetWindowPos(m_hwnd, nullptr, posX, posY, winW, winH, SWP_NOZORDER);

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
    if (btnId == ID_BTN_CONNECT) {
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
    std::wcsncpy(nid.szTip, L"WinMirror Screen Mirroring", ARRAYSIZE(nid.szTip) - 1);
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
    AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT, L"Quit WinMirror");

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
    ensureTextScratch(screenDc, cw, sc(96, m_dpi));

    WinMirrorStatus snap;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        snap = m_status;
    }

    // Translucent dark navy-indigo base over DWM Acrylic blur (opaque fallback when DWM disabled)
    glass::fillCanvasGlass(m_canvasPx, cw, ch, m_glassBlurActive ? 104 : 255);

    const GlassColor cWhite = {246, 248, 255, 255};
    const GlassColor cMuted = {166, 178, 208, 230};
    const GlassColor cCyan  = {88, 212, 255, 255};
    const GlassColor cGreen = {64, 224, 158, 255};
    const GlassColor cAmber = {252, 200, 72, 255};

    // Header: Glowing Mirroring Icon + Title + Subtitle + Status Pill
    drawMirroringGlyph(m_canvasPx, cw, ch, scf(22, m_dpi), scf(20, m_dpi), m_dpi);
    drawTextGlass({ sc(56, m_dpi), sc(14, m_dpi), sc(292, m_dpi), sc(39, m_dpi) },
                  m_fontTitle, cWhite, L"Screen Mirroring",
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    drawTextGlass({ sc(56, m_dpi), sc(38, m_dpi), sc(292, m_dpi), sc(56, m_dpi) },
                  m_fontSmall, cMuted, L"Mirror your screen in high quality",
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    glass::fillRoundRectGrad(m_canvasPx, cw, ch,
                             scf(298, m_dpi), scf(20, m_dpi), scf(424, m_dpi), scf(46, m_dpi),
                             scf(13, m_dpi),
                             snap.clients > 0 ? GlassColor{24, 68, 56, 165} : GlassColor{34, 40, 62, 145},
                             snap.clients > 0 ? GlassColor{18, 52, 46, 165} : GlassColor{26, 32, 52, 145},
                             snap.clients > 0 ? GlassColor{68, 210, 150, 190} : GlassColor{150, 168, 220, 95}, 1.0f);
    glass::fillAaCircle(m_canvasPx, cw, ch, scf(313, m_dpi), scf(33, m_dpi), scf(3.8f, m_dpi),
                        snap.clients > 0 ? cGreen : (snap.streamingReady ? cCyan : cAmber));

    wchar_t pillTxt[48] = {};
    if (snap.clients > 0) {
        std::swprintf(pillTxt, ARRAYSIZE(pillTxt), L"Live \u2022 %.0f FPS", snap.actualFps);
    } else if (snap.streamingReady) {
        std::swprintf(pillTxt, ARRAYSIZE(pillTxt), L"Ready \u2022 Wi-Fi");
    } else {
        std::swprintf(pillTxt, ARRAYSIZE(pillTxt), L"Starting...");
    }
    drawTextGlass({ sc(323, m_dpi), sc(20, m_dpi), sc(416, m_dpi), sc(46, m_dpi) },
                  m_fontSmall, cWhite, pillTxt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Central Frosted Receiver Card Container
    glass::fillRoundRectGrad(m_canvasPx, cw, ch,
                             scf(18, m_dpi), scf(68, m_dpi), scf(424, m_dpi), scf(264, m_dpi),
                             scf(16, m_dpi),
                             {34, 38, 60, 122}, {22, 26, 44, 134},
                             {170, 188, 232, 90}, 1.0f);

    if (snap.receivers.empty()) {
        drawLaptopGlyph(m_canvasPx, cw, ch, scf(208, m_dpi), scf(130, m_dpi), m_dpi, cMuted);
        drawTextGlass({ sc(36, m_dpi), sc(154, m_dpi), sc(406, m_dpi), sc(178, m_dpi) },
                      m_fontRow, cWhite, L"Looking for AirPlay devices...",
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        drawTextGlass({ sc(36, m_dpi), sc(178, m_dpi), sc(406, m_dpi), sc(200, m_dpi) },
                      m_fontSmall, cMuted, L"Open OBS on MacBook or enter Mac IP below",
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
                    scf(12, m_dpi),
                    hovered ? GlassColor{96, 94, 212, 185} : GlassColor{84, 82, 196, 175},
                    hovered ? GlassColor{62, 84, 176, 180} : GlassColor{52, 72, 152, 170},
                    {136, 174, 255, 215}, 1.15f);
            } else {
                glass::fillRoundRectGrad(
                    m_canvasPx, cw, ch,
                    static_cast<float>(row.left), static_cast<float>(row.top),
                    static_cast<float>(row.right), static_cast<float>(row.bottom),
                    scf(12, m_dpi),
                    hovered ? GlassColor{68, 76, 112, 155} : GlassColor{38, 44, 70, 96},
                    hovered ? GlassColor{52, 60, 96, 155} : GlassColor{28, 34, 58, 96},
                    hovered ? GlassColor{156, 176, 232, 155} : GlassColor{132, 148, 192, 62}, 1.0f);
            }

            GlassColor iconCol = dev.isConnected ? cCyan : (dev.isDirectObs ? cWhite : cMuted);
            if (dev.isLaptop) {
                drawLaptopGlyph(m_canvasPx, cw, ch,
                                static_cast<float>(row.left + sc(16, m_dpi)),
                                static_cast<float>(row.top + sc(18, m_dpi)), m_dpi, iconCol);
            } else {
                drawTvGlyph(m_canvasPx, cw, ch,
                            static_cast<float>(row.left + sc(16, m_dpi)),
                            static_cast<float>(row.top + sc(18, m_dpi)), m_dpi, iconCol);
            }

            drawTextGlass({ row.left + sc(54, m_dpi), row.top + sc(6, m_dpi),
                            row.right - sc(106, m_dpi), row.top + sc(29, m_dpi) },
                          m_fontRow, cWhite, utf8ToWide(dev.name),
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            wchar_t subLine[96] = {};
            if (dev.isConnected) {
                std::swprintf(subLine, ARRAYSIZE(subLine), L"%u x %u  \u2022  %ls  \u2022  %ls",
                              snap.width ? snap.width : 1920,
                              snap.height ? snap.height : 1080,
                              snap.isHevc ? L"HEVC" : L"H.264",
                              snap.audioActive ? L"Audio ON" : L"Muted");
            } else if (dev.isDirectObs) {
                std::swprintf(subLine, ARRAYSIZE(subLine), L"OBS Ready  \u2022  %hs  \u2022  Video + Audio", dev.ip.c_str());
            } else {
                std::swprintf(subLine, ARRAYSIZE(subLine), L"AirPlay  \u2022  %hs", dev.ip.c_str());
            }
            drawTextGlass({ row.left + sc(54, m_dpi), row.top + sc(29, m_dpi),
                            row.right - sc(106, m_dpi), row.top + sc(48, m_dpi) },
                          m_fontSmall, cMuted, subLine,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            if (dev.isConnected) {
                float bx0 = static_cast<float>(row.right - sc(100, m_dpi));
                float by0 = static_cast<float>(row.top + sc(14, m_dpi));
                float bx1 = static_cast<float>(row.right - sc(12, m_dpi));
                float by1 = static_cast<float>(row.bottom - sc(14, m_dpi));
                glass::fillRoundRectGrad(m_canvasPx, cw, ch, bx0, by0, bx1, by1, scf(13, m_dpi),
                                         {34, 128, 92, 225}, {24, 102, 74, 225}, {76, 222, 160, 215}, 1.0f);
                drawCheckGlyph(m_canvasPx, cw, ch, bx0 + scf(11, m_dpi), by0 + scf(9, m_dpi), m_dpi, cWhite);
                drawTextGlass({ static_cast<LONG>(bx0) + sc(24, m_dpi), static_cast<LONG>(by0),
                                static_cast<LONG>(bx1) - sc(8, m_dpi), static_cast<LONG>(by1) },
                              m_fontSmall, cWhite, L"Mirroring", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            } else if (dev.isDirectObs) {
                float bx0 = static_cast<float>(row.right - sc(68, m_dpi));
                float by0 = static_cast<float>(row.top + sc(16, m_dpi));
                float bx1 = static_cast<float>(row.right - sc(14, m_dpi));
                float by1 = static_cast<float>(row.bottom - sc(16, m_dpi));
                glass::fillRoundRectGrad(m_canvasPx, cw, ch, bx0, by0, bx1, by1, scf(9, m_dpi),
                                         {52, 82, 160, 175}, {44, 68, 138, 175}, {122, 162, 255, 165}, 1.0f);
                drawTextGlass({ static_cast<LONG>(bx0), static_cast<LONG>(by0),
                                static_cast<LONG>(bx1), static_cast<LONG>(by1) },
                              m_fontLabel, cWhite, L"OBS", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                drawTextGlass({ row.right - sc(74, m_dpi), row.top, row.right - sc(14, m_dpi), row.bottom },
                              m_fontSmall, cMuted, L"AirPlay", DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
        }
    }

    // Middle Action Row: Frosted Manual IP Pill + Connect Gradient Pill + Scan Glass Pill
    const auto ctrls = buildControlRects(m_dpi);
    auto rectOf = [&](int id) -> RECT {
        for (const auto& c : ctrls) if (c.id == id) return c.rc;
        return {};
    };

    // 1) Left Capsule (Mac IP Input / Auto display)
    RECT rcIp = rectOf(ID_IP_BOX);
    glass::fillRoundRectGrad(m_canvasPx, cw, ch,
                             static_cast<float>(rcIp.left), static_cast<float>(rcIp.top),
                             static_cast<float>(rcIp.right), static_cast<float>(rcIp.bottom),
                             scf(11, m_dpi),
                             {30, 34, 54, 150}, {24, 28, 46, 150},
                             m_ipFocused ? GlassColor{96, 182, 255, 225}
                                         : (m_hoverBtnId == ID_IP_BOX ? GlassColor{156, 174, 226, 145}
                                                                      : GlassColor{136, 152, 198, 85}),
                             m_ipFocused ? 1.3f : 1.0f);
    drawMonitorGlyph(m_canvasPx, cw, ch,
                     static_cast<float>(rcIp.left + sc(12, m_dpi)),
                     static_cast<float>(rcIp.top + sc(11, m_dpi)), m_dpi,
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
        if (!autoIp.empty()) {
            ipLabel = L"Mac IP: " + utf8ToWide(autoIp) + L" (edit)";
        } else {
            ipLabel = L"Enter Mac IP (or click above)";
        }
        ipTextCol = cMuted;
    }
    drawTextGlass({ rcIp.left + sc(34, m_dpi), rcIp.top, rcIp.right - sc(10, m_dpi), rcIp.bottom },
                  m_fontBody, ipTextCol, ipLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // 2) Connect Gradient Pill
    RECT rcConn = rectOf(ID_BTN_CONNECT);
    bool connHv = (m_hoverBtnId == ID_BTN_CONNECT);
    bool connDn = (m_pressedBtnId == ID_BTN_CONNECT);
    glass::fillRoundRectGrad(m_canvasPx, cw, ch,
                             static_cast<float>(rcConn.left), static_cast<float>(rcConn.top),
                             static_cast<float>(rcConn.right), static_cast<float>(rcConn.bottom),
                             scf(12, m_dpi),
                             connDn ? GlassColor{48, 96, 212, 245} : (connHv ? GlassColor{78, 146, 255, 250} : GlassColor{60, 128, 252, 245}),
                             connDn ? GlassColor{104, 62, 198, 245} : (connHv ? GlassColor{154, 102, 255, 250} : GlassColor{134, 82, 244, 245}),
                             {178, 198, 255, 190}, 1.0f);
    glass::fillAaPlayTriangle(m_canvasPx, cw, ch,
                              static_cast<float>(rcConn.left + sc(20, m_dpi)),
                              static_cast<float>((rcConn.top + rcConn.bottom) / 2),
                              scf(9.5f, m_dpi), cWhite);
    drawTextGlass({ rcConn.left + sc(30, m_dpi), rcConn.top, rcConn.right - sc(8, m_dpi), rcConn.bottom },
                  m_fontBody, cWhite, L"Connect", DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 3) Scan Frosted Pill
    RECT rcScan = rectOf(ID_BTN_SCAN);
    bool scanHv = (m_hoverBtnId == ID_BTN_SCAN);
    glass::fillRoundRectGrad(m_canvasPx, cw, ch,
                             static_cast<float>(rcScan.left), static_cast<float>(rcScan.top),
                             static_cast<float>(rcScan.right), static_cast<float>(rcScan.bottom),
                             scf(12, m_dpi),
                             scanHv ? GlassColor{58, 66, 98, 185} : GlassColor{38, 44, 68, 160},
                             scanHv ? GlassColor{44, 52, 82, 185} : GlassColor{28, 34, 56, 160},
                             {156, 174, 224, scanHv ? uint8_t(165) : uint8_t(95)}, 1.0f);
    drawRefreshGlyph(m_canvasPx, cw, ch,
                     static_cast<float>(rcScan.left + sc(19, m_dpi)),
                     static_cast<float>((rcScan.top + rcScan.bottom) / 2), m_dpi, cWhite);
    drawTextGlass({ rcScan.left + sc(30, m_dpi), rcScan.top, rcScan.right - sc(6, m_dpi), rcScan.bottom },
                  m_fontBody, cWhite, L"Scan", DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Telemetry Spec Strip with vertical dividers
    int specY0 = sc(328, m_dpi), specY1 = sc(348, m_dpi);
    drawChipGlyph(m_canvasPx, cw, ch, scf(22, m_dpi), scf(332, m_dpi), m_dpi, cMuted);
    drawTextGlass({ sc(40, m_dpi), specY0, sc(96, m_dpi), specY1 },
                  m_fontSmall, cWhite, snap.isHevc ? L"HEVC" : L"H.264", DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    glass::drawAaSegment(m_canvasPx, cw, ch, scf(102, m_dpi), scf(331, m_dpi), scf(102, m_dpi), scf(345, m_dpi), 1.0f, {150, 165, 210, 80});
    drawGaugeGlyph(m_canvasPx, cw, ch, scf(118, m_dpi), scf(338, m_dpi), m_dpi, cMuted);
    wchar_t fpsSpec[32] = {};
    std::swprintf(fpsSpec, ARRAYSIZE(fpsSpec), L"%.0f FPS", snap.clients > 0 ? snap.actualFps : static_cast<float>(snap.targetFps));
    drawTextGlass({ sc(131, m_dpi), specY0, sc(198, m_dpi), specY1 },
                  m_fontSmall, cWhite, fpsSpec, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    glass::drawAaSegment(m_canvasPx, cw, ch, scf(204, m_dpi), scf(331, m_dpi), scf(204, m_dpi), scf(345, m_dpi), 1.0f, {150, 165, 210, 80});
    glass::fillAaCircle(m_canvasPx, cw, ch, scf(218, m_dpi), scf(338, m_dpi), scf(3.0f, m_dpi), snap.clients > 0 ? cCyan : cMuted);
    wchar_t mbpsSpec[40] = {};
    if (snap.clients > 0) {
        std::swprintf(mbpsSpec, ARRAYSIZE(mbpsSpec), L"%.1f Mbps", snap.actualMbps);
    } else {
        std::swprintf(mbpsSpec, ARRAYSIZE(mbpsSpec), L"%u Mbps", snap.targetMbps ? snap.targetMbps : 200);
    }
    drawTextGlass({ sc(228, m_dpi), specY0, sc(312, m_dpi), specY1 },
                  m_fontSmall, cWhite, mbpsSpec, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    glass::drawAaSegment(m_canvasPx, cw, ch, scf(318, m_dpi), scf(331, m_dpi), scf(318, m_dpi), scf(345, m_dpi), 1.0f, {150, 165, 210, 80});
    drawSpeakerGlyph(m_canvasPx, cw, ch, scf(332, m_dpi), scf(332, m_dpi), m_dpi, snap.audioActive ? cWhite : cMuted);
    drawTextGlass({ sc(350, m_dpi), specY0, sc(420, m_dpi), specY1 },
                  m_fontSmall, snap.audioActive ? cWhite : cMuted,
                  snap.audioActive ? L"Audio" : L"Muted", DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Bottom Live Status Line
    glass::fillAaCircle(m_canvasPx, cw, ch, scf(26, m_dpi), scf(374, m_dpi), scf(4.2f, m_dpi),
                        snap.clients > 0 ? cGreen : cCyan);
    drawTextGlass({ sc(38, m_dpi), sc(361, m_dpi), sc(424, m_dpi), sc(387, m_dpi) },
                  m_fontSmall, snap.clients > 0 ? cWhite : cMuted,
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
