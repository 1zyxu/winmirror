#include "GuiWindow.h"
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <vector>

namespace {

constexpr UINT WM_APP_REFRESH = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;
constexpr UINT_PTR TIMER_COPY_RESET = 101;

constexpr int ID_BTN_FPS   = 1001;
constexpr int ID_BTN_BOOST = 1002;
constexpr int ID_BTN_VDD   = 1003;
constexpr int ID_BTN_COPY  = 1004;
constexpr int ID_BTN_TRAY  = 1005;

constexpr int ID_TRAY_OPEN  = 2001;
constexpr int ID_TRAY_FPS   = 2002;
constexpr int ID_TRAY_BOOST = 2003;
constexpr int ID_TRAY_QUIT  = 2004;

constexpr int BASE_W = 580;
constexpr int BASE_H = 506;

constexpr COLORREF COL_BG        = RGB(13, 17, 23);
constexpr COLORREF COL_CARD      = RGB(21, 26, 35);
constexpr COLORREF COL_BORDER    = RGB(38, 47, 63);
constexpr COLORREF COL_TEXT      = RGB(242, 246, 252);
constexpr COLORREF COL_MUTED     = RGB(139, 149, 167);
constexpr COLORREF COL_ACCENT    = RGB(37, 99, 235);
constexpr COLORREF COL_ACCENT_HV = RGB(59, 130, 246);
constexpr COLORREF COL_CYAN      = RGB(56, 189, 248);
constexpr COLORREF COL_GREEN     = RGB(52, 211, 153);
constexpr COLORREF COL_AMBER     = RGB(251, 191, 36);
constexpr COLORREF COL_PURPLE    = RGB(167, 139, 250);
constexpr COLORREF COL_BTN       = RGB(29, 36, 48);
constexpr COLORREF COL_BTN_HV    = RGB(44, 54, 72);

int sc(int px, UINT dpi) {
    return MulDiv(px, static_cast<int>(dpi), 96);
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], len);
    return w;
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

void enableDarkTitleBar(HWND hwnd) {
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        using PfnDwmSetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        auto fn = reinterpret_cast<PfnDwmSetAttr>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
        if (fn) {
            BOOL dark = TRUE;
            if (FAILED(fn(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark)))) {
                fn(hwnd, 19, &dark, sizeof(dark));
            }
        }
        FreeLibrary(dwm);
    }
}

void adjustRectForWindowDpi(RECT& rc, DWORD style, DWORD exStyle, UINT dpi) {
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        using PfnAdj = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
        auto fn = reinterpret_cast<PfnAdj>(GetProcAddress(u32, "AdjustWindowRectExForDpi"));
        if (fn && fn(&rc, style, FALSE, exStyle, dpi)) return;
    }
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
}

void fillRoundRect(HDC hdc, const RECT& r, int radius, COLORREF fill, COLORREF border) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBr = SelectObject(hdc, br);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(br);
    DeleteObject(pen);
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
            bool inside = (dx * dx + dy * dy <= 25);
            if (!inside) {
                px[y * sz + x] = 0x00000000;
                continue;
            }
            // Outer gradient blue->indigo + inner glowing screen frame + play/cast triangle
            uint32_t col = (y < 16) ? 0xFF2563EB : 0xFF1D4ED8;
            if (x >= 6 && x <= 25 && y >= 8 && y <= 21 &&
                (x == 6 || x == 25 || y == 8 || y == 21)) {
                col = 0xFFFFFFFF;
            } else if (x >= 11 && x <= 20 && y == 24) {
                col = 0xFF93C5FD;
            } else if (x >= 13 && x <= 19 && y >= 12 && y <= 17 && (x - 13) >= std::abs(y - 14) / 1) {
                if (std::abs(y - 14) <= (19 - x) / 2) col = 0xFF34D399;
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
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

LRESULT CALLBACK buttonHoverSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WNDPROC orig = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_MOUSEMOVE) {
        SendMessageW(GetParent(hwnd), WM_USER + 50, GetDlgCtrlID(hwnd), 1);
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
    } else if (msg == WM_MOUSELEAVE) {
        SendMessageW(GetParent(hwnd), WM_USER + 50, GetDlgCtrlID(hwnd), 0);
    } else if (msg == WM_SETCURSOR) {
        SetCursor(LoadCursorA(nullptr, IDC_HAND));
        return TRUE;
    }
    return CallWindowProcW(orig, hwnd, msg, wParam, lParam);
}

} // namespace

WinMirrorGui::WinMirrorGui() = default;

WinMirrorGui::~WinMirrorGui() {
    removeTrayIcon();
    if (m_appIcon) { DestroyIcon(m_appIcon); m_appIcon = nullptr; }
    if (m_fontTitle) { DeleteObject(m_fontTitle); m_fontTitle = nullptr; }
    if (m_fontMetric) { DeleteObject(m_fontMetric); m_fontMetric = nullptr; }
    if (m_fontLabel) { DeleteObject(m_fontLabel); m_fontLabel = nullptr; }
    if (m_fontBody) { DeleteObject(m_fontBody); m_fontBody = nullptr; }
    if (m_fontSmall) { DeleteObject(m_fontSmall); m_fontSmall = nullptr; }
}

void WinMirrorGui::setActions(std::function<void()> onToggleUnlimitedFps,
                              std::function<void()> onToggleGameBoost,
                              std::function<void()> onInstallVdd,
                              std::function<void()> onQuit) {
    m_onToggleUnlimited = std::move(onToggleUnlimitedFps);
    m_onToggleGameBoost = std::move(onToggleGameBoost);
    m_onInstallVdd = std::move(onInstallVdd);
    m_onQuit = std::move(onQuit);
}

void WinMirrorGui::rebuildFonts() {
    if (m_fontTitle) DeleteObject(m_fontTitle);
    if (m_fontMetric) DeleteObject(m_fontMetric);
    if (m_fontLabel) DeleteObject(m_fontLabel);
    if (m_fontBody) DeleteObject(m_fontBody);
    if (m_fontSmall) DeleteObject(m_fontSmall);

    m_fontTitle  = makeUiFont(15, m_dpi, FW_SEMIBOLD);
    m_fontMetric = makeUiFont(17, m_dpi, FW_BOLD);
    m_fontLabel  = makeUiFont(8,  m_dpi, FW_SEMIBOLD);
    m_fontBody   = makeUiFont(9,  m_dpi, FW_NORMAL);
    m_fontSmall  = makeUiFont(8,  m_dpi, FW_NORMAL);
}

bool WinMirrorGui::createAndShow() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    const wchar_t* clsName = L"WinMirrorDashboardWnd";

    m_appIcon = createAppBadgeIcon();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProcThunk;
    wc.hInstance = hInst;
    wc.hIcon = m_appIcon;
    wc.hIconSm = m_appIcon;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = clsName;
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const DWORD exStyle = WS_EX_APPWINDOW;

    m_hwnd = CreateWindowExW(
        exStyle, clsName, L"WinMirror AirPlay", style,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 500,
        nullptr, nullptr, hInst, this);
    if (!m_hwnd) return false;

    m_dpi = queryWindowDpi(m_hwnd);
    rebuildFonts();
    enableDarkTitleBar(m_hwnd);

    RECT rc = { 0, 0, sc(BASE_W, m_dpi), sc(BASE_H, m_dpi) };
    adjustRectForWindowDpi(rc, style, exStyle, m_dpi);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;

    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int posX = work.left + ((work.right - work.left) - winW) / 2;
    int posY = work.top + ((work.bottom - work.top) - winH) / 2;
    SetWindowPos(m_hwnd, nullptr, posX, posY, winW, winH, SWP_NOZORDER);

    auto makeBtn = [&](int id, const wchar_t* text) -> HWND {
        HWND b = CreateWindowExW(0, L"BUTTON", text,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                 0, 0, 100, 32, m_hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 hInst, nullptr);
        WNDPROC prev = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(b, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(buttonHoverSubclass)));
        SetWindowLongPtrW(b, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(prev));
        return b;
    };

    m_btnFps   = makeBtn(ID_BTN_FPS,   L"FPS: Native");
    m_btnBoost = makeBtn(ID_BTN_BOOST, L"Game Boost: ON");
    m_btnVdd   = makeBtn(ID_BTN_VDD,   L"Install 4K HDR");
    m_btnCopy  = makeBtn(ID_BTN_COPY,  L"Copy IP");
    m_btnTray  = makeBtn(ID_BTN_TRAY,  L"Hide Tray");

    layoutChildren();
    addTrayIcon();
    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    return true;
}

void WinMirrorGui::layoutChildren() {
    if (!m_hwnd) return;
    int y = sc(420, m_dpi);
    int h = sc(36, m_dpi);
    SetWindowPos(m_btnFps,   nullptr, sc(20,  m_dpi), y, sc(102, m_dpi), h, SWP_NOZORDER);
    SetWindowPos(m_btnBoost, nullptr, sc(131, m_dpi), y, sc(108, m_dpi), h, SWP_NOZORDER);
    SetWindowPos(m_btnVdd,   nullptr, sc(248, m_dpi), y, sc(114, m_dpi), h, SWP_NOZORDER);
    SetWindowPos(m_btnCopy,  nullptr, sc(371, m_dpi), y, sc(92,  m_dpi), h, SWP_NOZORDER);
    SetWindowPos(m_btnTray,  nullptr, sc(472, m_dpi), y, sc(88,  m_dpi), h, SWP_NOZORDER);
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

void WinMirrorGui::addTrayIcon() {
    if (m_trayAdded || !m_hwnd) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = m_appIcon;
    std::wcsncpy(nid.szTip, L"WinMirror AirPlay (Max 200 Mbps)", ARRAYSIZE(nid.szTip) - 1);
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
    m_hiddenToTray = false;
    ShowWindow(m_hwnd, SW_SHOW);
    ShowWindow(m_hwnd, SW_RESTORE);
    SetForegroundWindow(m_hwnd);
}

void WinMirrorGui::hideToTray() {
    if (!m_hwnd) return;
    addTrayIcon();
    m_hiddenToTray = true;
    ShowWindow(m_hwnd, SW_HIDE);
}

void WinMirrorGui::showTrayContextMenu() {
    if (!m_hwnd) return;
    WinMirrorStatus snap;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        snap = m_status;
    }
    POINT pt = {};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, L"Open WinMirror Dashboard");
    AppendMenuW(menu, MF_STRING | (snap.unlimitedFps ? MF_CHECKED : 0), ID_TRAY_FPS, L"Unlimited FPS Mode");
    AppendMenuW(menu, MF_STRING | (snap.gameBoost ? MF_CHECKED : 0), ID_TRAY_BOOST, L"Game Low-Latency Boost");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT, L"Quit WinMirror");

    SetForegroundWindow(m_hwnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                              pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == ID_TRAY_OPEN) {
        showFromTray();
    } else if (cmd == ID_TRAY_FPS) {
        if (m_onToggleUnlimited) m_onToggleUnlimited();
    } else if (cmd == ID_TRAY_BOOST) {
        if (m_onToggleGameBoost) m_onToggleGameBoost();
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
    m_copiedFlash = true;
    SetTimer(m_hwnd, TIMER_COPY_RESET, 1800, nullptr);
    InvalidateRect(m_btnCopy, nullptr, FALSE);
}

void WinMirrorGui::drawButton(const DRAWITEMSTRUCT* dis) {
    if (!dis) return;
    int id = static_cast<int>(dis->CtlID);
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    bool hovered = (m_hoverBtnId == id);
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;

    WinMirrorStatus snap;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        snap = m_status;
    }

    COLORREF bg = hovered ? COL_BTN_HV : COL_BTN;
    COLORREF bd = COL_BORDER;
    COLORREF fg = disabled ? COL_MUTED : COL_TEXT;

    std::wstring label;
    if (id == ID_BTN_FPS) {
        label = snap.unlimitedFps ? L"FPS: Unlim" : L"FPS: Native";
        if (snap.unlimitedFps) {
            bg = hovered ? COL_ACCENT_HV : COL_ACCENT;
            bd = COL_ACCENT_HV;
        }
    } else if (id == ID_BTN_BOOST) {
        label = snap.gameBoost ? L"Boost: ON" : L"Boost: Std";
        if (snap.gameBoost) {
            bg = hovered ? RGB(20, 83, 58) : RGB(16, 65, 45);
            bd = COL_GREEN;
            fg = COL_GREEN;
        }
    } else if (id == ID_BTN_VDD) {
        if (snap.installingVdd) {
            label = L"Installing...";
            fg = COL_AMBER;
        } else if (snap.hasVdd) {
            label = L"4K HDR Ready";
            fg = COL_GREEN;
        } else {
            label = L"Install 4K HDR";
        }
    } else if (id == ID_BTN_COPY) {
        label = m_copiedFlash ? L"Copied!" : L"Copy IP";
        if (m_copiedFlash) {
            fg = COL_GREEN;
            bd = COL_GREEN;
        }
    } else if (id == ID_BTN_TRAY) {
        label = L"Hide Tray";
    }

    if (pressed) {
        bg = RGB(24, 30, 40);
    }

    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    HBRUSH clrBr = CreateSolidBrush(COL_BG);
    FillRect(hdc, &rc, clrBr);
    DeleteObject(clrBr);

    fillRoundRect(hdc, rc, sc(8, m_dpi), bg, bd);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fg);
    HGDIOBJ oldFont = SelectObject(hdc, m_fontBody);
    DrawTextW(hdc, label.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
}

void WinMirrorGui::paintWindow(HWND hwnd) {
    PAINTSTRUCT ps = {};
    HDC screenDc = BeginPaint(hwnd, &ps);

    RECT client = {};
    GetClientRect(hwnd, &client);
    int cw = client.right - client.left;
    int ch = client.bottom - client.top;

    HDC memDc = CreateCompatibleDC(screenDc);
    HBITMAP memBmp = CreateCompatibleBitmap(screenDc, std::max(1, cw), std::max(1, ch));
    HGDIOBJ oldBmp = SelectObject(memDc, memBmp);

    HBRUSH bgBr = CreateSolidBrush(COL_BG);
    FillRect(memDc, &client, bgBr);
    DeleteObject(bgBr);
    SetBkMode(memDc, TRANSPARENT);

    WinMirrorStatus snap;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        snap = m_status;
    }

    auto textOutRect = [&](const RECT& r, HFONT font, COLORREF col, const std::wstring& txt, UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
        HGDIOBJ oldF = SelectObject(memDc, font);
        SetTextColor(memDc, col);
        RECT tmp = r;
        DrawTextW(memDc, txt.c_str(), -1, &tmp, fmt);
        SelectObject(memDc, oldF);
    };

    // Header
    textOutRect({ sc(20, m_dpi), sc(14, m_dpi), sc(310, m_dpi), sc(42, m_dpi) },
                m_fontTitle, COL_TEXT, L"WinMirror AirPlay");
    wchar_t verLine[112] = {};
    std::swprintf(verLine, ARRAYSIZE(verLine), L"v%hs  |  Game Screen Low-Latency Studio (Max 200 Mbps)", WINMIRROR_VERSION);
    textOutRect({ sc(20, m_dpi), sc(40, m_dpi), sc(385, m_dpi), sc(58, m_dpi) },
                m_fontSmall, COL_MUTED, verLine);

    // Status pill
    RECT pill = { sc(392, m_dpi), sc(18, m_dpi), sc(560, m_dpi), sc(48, m_dpi) };
    COLORREF pillBg = snap.clients > 0 ? RGB(16, 42, 34) : (snap.streamingReady ? RGB(38, 31, 16) : RGB(27, 34, 48));
    COLORREF pillBd = snap.clients > 0 ? RGB(30, 90, 68) : (snap.streamingReady ? RGB(82, 62, 24) : COL_BORDER);
    COLORREF dotCol = snap.clients > 0 ? COL_GREEN : (snap.streamingReady ? COL_AMBER : COL_MUTED);
    fillRoundRect(memDc, pill, sc(14, m_dpi), pillBg, pillBd);

    HBRUSH dotBr = CreateSolidBrush(dotCol);
    HPEN dotPen = CreatePen(PS_SOLID, 1, dotCol);
    HGDIOBJ oBr = SelectObject(memDc, dotBr);
    HGDIOBJ oPn = SelectObject(memDc, dotPen);
    int dr = sc(8, m_dpi);
    int dx = pill.left + sc(12, m_dpi);
    int dy = (pill.top + pill.bottom - dr) / 2;
    Ellipse(memDc, dx, dy, dx + dr, dy + dr);
    SelectObject(memDc, oBr);
    SelectObject(memDc, oPn);
    DeleteObject(dotBr);
    DeleteObject(dotPen);

    wchar_t pillTxt[64] = {};
    if (snap.clients > 0) {
        std::swprintf(pillTxt, ARRAYSIZE(pillTxt), L"Live (%d Mac)", snap.clients);
    } else if (snap.streamingReady) {
        std::swprintf(pillTxt, ARRAYSIZE(pillTxt), L"Waiting for Mac");
    } else {
        std::swprintf(pillTxt, ARRAYSIZE(pillTxt), L"Initializing...");
    }
    textOutRect({ dx + dr + sc(8, m_dpi), pill.top, pill.right - sc(8, m_dpi), pill.bottom },
                m_fontBody, COL_TEXT, pillTxt);

    auto drawCardTopStripe = [&](const RECT& r, COLORREF accent) {
        RECT bar = { r.left + sc(12, m_dpi), r.top + sc(1, m_dpi), r.right - sc(12, m_dpi), r.top + sc(4, m_dpi) };
        HBRUSH ab = CreateSolidBrush(accent);
        FillRect(memDc, &bar, ab);
        DeleteObject(ab);
    };

    // 3 Top Metric Cards
    const int cardY = sc(70, m_dpi);
    const int cardH = sc(100, m_dpi);
    const int cardW = sc(172, m_dpi);
    const int radius = sc(10, m_dpi);

    // Card 1: FRAME PACING
    RECT c1 = { sc(20, m_dpi), cardY, sc(20, m_dpi) + cardW, cardY + cardH };
    fillRoundRect(memDc, c1, radius, COL_CARD, COL_BORDER);
    drawCardTopStripe(c1, COL_ACCENT_HV);
    textOutRect({ c1.left + sc(12, m_dpi), c1.top + sc(10, m_dpi), c1.right - sc(10, m_dpi), c1.top + sc(26, m_dpi) },
                m_fontLabel, COL_MUTED, L"FRAME PACING");
    wchar_t fpsMain[48] = {};
    if (!snap.streamingReady) {
        std::swprintf(fpsMain, ARRAYSIZE(fpsMain), L"-- FPS");
    } else {
        std::swprintf(fpsMain, ARRAYSIZE(fpsMain), L"%.1f FPS", snap.actualFps);
    }
    textOutRect({ c1.left + sc(12, m_dpi), c1.top + sc(27, m_dpi), c1.right - sc(10, m_dpi), c1.top + sc(56, m_dpi) },
                m_fontMetric, COL_TEXT, fpsMain);
    wchar_t fpsSub1[64] = {};
    std::swprintf(fpsSub1, ARRAYSIZE(fpsSub1), L"%ls (%uHz) | Enc %.1fms",
                  snap.unlimitedFps ? L"UNLIM" : L"Native", snap.targetFps, snap.encodeAvgMs);
    textOutRect({ c1.left + sc(12, m_dpi), c1.top + sc(59, m_dpi), c1.right - sc(10, m_dpi), c1.top + sc(75, m_dpi) },
                m_fontSmall, COL_CYAN, fpsSub1);
    wchar_t fpsSub2[64] = {};
    std::swprintf(fpsSub2, ARRAYSIZE(fpsSub2), L"%hs | Skips %u", snap.gpuMode.c_str(), snap.captureSkips);
    textOutRect({ c1.left + sc(12, m_dpi), c1.top + sc(76, m_dpi), c1.right - sc(10, m_dpi), c1.top + sc(92, m_dpi) },
                m_fontSmall, COL_MUTED, fpsSub2);

    // Card 2: BITRATE & CAPACITY METER (up to 200 Mbps)
    RECT c2 = { sc(204, m_dpi), cardY, sc(204, m_dpi) + cardW, cardY + cardH };
    fillRoundRect(memDc, c2, radius, COL_CARD, COL_BORDER);
    drawCardTopStripe(c2, snap.bitrateScalePct < 100 ? COL_AMBER : COL_GREEN);
    textOutRect({ c2.left + sc(12, m_dpi), c2.top + sc(10, m_dpi), c2.right - sc(10, m_dpi), c2.top + sc(26, m_dpi) },
                m_fontLabel, COL_MUTED, L"THROUGHPUT (MAX 200)");
    wchar_t mbpsMain[48] = {};
    if (!snap.streamingReady) {
        std::swprintf(mbpsMain, ARRAYSIZE(mbpsMain), L"-- Mbps");
    } else {
        std::swprintf(mbpsMain, ARRAYSIZE(mbpsMain), L"%.1f Mbps", snap.actualMbps);
    }
    textOutRect({ c2.left + sc(12, m_dpi), c2.top + sc(27, m_dpi), c2.right - sc(10, m_dpi), c2.top + sc(56, m_dpi) },
                m_fontMetric, COL_TEXT, mbpsMain);

    // Progress meter inside Card 2 (0..200 Mbps scale)
    RECT barTrack = { c2.left + sc(12, m_dpi), c2.top + sc(62, m_dpi), c2.right - sc(12, m_dpi), c2.top + sc(68, m_dpi) };
    HBRUSH trackBr = CreateSolidBrush(RGB(34, 42, 56));
    FillRect(memDc, &barTrack, trackBr);
    DeleteObject(trackBr);
    double pctFill = std::min(1.0, std::max(0.0, static_cast<double>(snap.actualMbps) / 200.0));
    if (pctFill > 0.0) {
        RECT barFill = barTrack;
        barFill.right = barTrack.left + static_cast<int>((barTrack.right - barTrack.left) * pctFill);
        HBRUSH fillBr = CreateSolidBrush(snap.bitrateScalePct < 100 ? COL_AMBER : COL_GREEN);
        FillRect(memDc, &barFill, fillBr);
        DeleteObject(fillBr);
    }

    wchar_t mbpsSub[64] = {};
    std::swprintf(mbpsSub, ARRAYSIZE(mbpsSub), L"Target %uM (%d%%) | %.0fKB",
                  snap.targetMbps, snap.bitrateScalePct, snap.avgFrameKb);
    textOutRect({ c2.left + sc(12, m_dpi), c2.top + sc(75, m_dpi), c2.right - sc(10, m_dpi), c2.top + sc(92, m_dpi) },
                m_fontSmall, snap.bitrateScalePct < 100 ? COL_AMBER : COL_GREEN, mbpsSub);

    // Card 3: GAME DISPLAY
    RECT c3 = { sc(388, m_dpi), cardY, sc(388, m_dpi) + cardW, cardY + cardH };
    fillRoundRect(memDc, c3, radius, COL_CARD, COL_BORDER);
    drawCardTopStripe(c3, snap.hdrActive ? COL_PURPLE : COL_CYAN);
    textOutRect({ c3.left + sc(12, m_dpi), c3.top + sc(10, m_dpi), c3.right - sc(10, m_dpi), c3.top + sc(26, m_dpi) },
                m_fontLabel, COL_MUTED, L"GAME DISPLAY");
    wchar_t resMain[48] = {};
    if (snap.width > 0 && snap.height > 0) {
        std::swprintf(resMain, ARRAYSIZE(resMain), L"%ux%u", snap.width, snap.height);
    } else {
        std::swprintf(resMain, ARRAYSIZE(resMain), L"Detecting...");
    }
    textOutRect({ c3.left + sc(12, m_dpi), c3.top + sc(27, m_dpi), c3.right - sc(10, m_dpi), c3.top + sc(56, m_dpi) },
                m_fontMetric, COL_TEXT, resMain);
    std::wstring dispSub1 = std::wstring(snap.isHevc ? L"HEVC " : L"H.264 ") +
                            (snap.hdrActive ? L"10-bit HDR10 PQ" : L"8-bit SDR BT.709");
    textOutRect({ c3.left + sc(12, m_dpi), c3.top + sc(59, m_dpi), c3.right - sc(10, m_dpi), c3.top + sc(75, m_dpi) },
                m_fontSmall, snap.hdrActive ? COL_PURPLE : COL_TEXT, dispSub1);
    wchar_t dispSub2[48] = {};
    std::swprintf(dispSub2, ARRAYSIZE(dispSub2), L"Panel %u Hz | IDR %u", snap.monitorHz, snap.keyframesSent);
    textOutRect({ c3.left + sc(12, m_dpi), c3.top + sc(76, m_dpi), c3.right - sc(10, m_dpi), c3.top + sc(92, m_dpi) },
                m_fontSmall, COL_MUTED, dispSub2);

    // System & Game Telemetry Panel (6 Rows)
    RECT infoCard = { sc(20, m_dpi), sc(182, m_dpi), sc(560, m_dpi), sc(404, m_dpi) };
    fillRoundRect(memDc, infoCard, radius, COL_CARD, COL_BORDER);

    auto drawRow = [&](int rowIdx, const wchar_t* k, const std::wstring& v, COLORREF vCol = COL_TEXT) {
        int ry = infoCard.top + sc(12 + rowIdx * 34, m_dpi);
        textOutRect({ infoCard.left + sc(16, m_dpi), ry, infoCard.left + sc(122, m_dpi), ry + sc(24, m_dpi) },
                    m_fontLabel, COL_MUTED, k);
        textOutRect({ infoCard.left + sc(126, m_dpi), ry, infoCard.right - sc(14, m_dpi), ry + sc(24, m_dpi) },
                    m_fontBody, vCol, v.empty() ? L"..." : v);
    };

    std::wstring encVal = utf8ToWide(snap.encoderName);
    wchar_t encExtra[64] = {};
    std::swprintf(encExtra, ARRAYSIZE(encExtra), L"  |  CBR %ls  |  %.1fms",
                  snap.gameBoost ? L"Low-Lat" : L"Std", snap.encodeAvgMs);
    if (!encVal.empty()) encVal += encExtra;

    std::wstring capVal = utf8ToWide(snap.captureName);
    if (!snap.pixelFormat.empty()) {
        capVal += L" (" + utf8ToWide(snap.pixelFormat) + L")";
    }
    std::wstring optVal = snap.gameBoost
        ? (std::wstring(L"HIGH Priority | MMCSS Games | 0.5ms Timer | ") +
           (snap.ecoQosOff ? L"EcoQoS Off" : L"Foreground"))
        : L"Standard Priority | Balanced VBV Buffer";
    std::wstring audVal = snap.audioActive
        ? (utf8ToWide(snap.audioName) + L" [" + std::to_wstring(snap.audioRate / 1000) + L" kHz Stereo]")
        : L"Loopback Off";
    std::wstring netVal = utf8ToWide(snap.endpointsSummary);
    std::wstring sysVal = (snap.dllLoaded ? (L"DLL v" + utf8ToWide(snap.dllVersion)) : L"1ms Timer") +
                          L" | FW:" + (snap.firewallOk ? L"OK" : L"Std") +
                          L" | BT:" + utf8ToWide(snap.btStatus) +
                          L" | 4K VDD:" + (snap.hasVdd ? L"Ready" : L"Missing");

    drawRow(0, L"ENCODER",    encVal);
    drawRow(1, L"CAPTURE",    capVal);
    drawRow(2, L"GAME OPT",   optVal, snap.gameBoost ? COL_GREEN : COL_MUTED);
    drawRow(3, L"AUDIO",      audVal);
    drawRow(4, L"TRANSPORTS", netVal, RGB(147, 197, 253));
    drawRow(5, L"SYSTEM",     sysVal, snap.hasVdd ? COL_GREEN : COL_MUTED);

    // Subtle status footer bar below buttons
    textOutRect({ sc(20, m_dpi), sc(468, m_dpi), sc(560, m_dpi), sc(494, m_dpi) },
                m_fontSmall, COL_MUTED, utf8ToWide(snap.statusBanner));

    BitBlt(screenDc, 0, 0, cw, ch, memDc, 0, 0, SRCCOPY);
    SelectObject(memDc, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDc);
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
        case WM_DRAWITEM:
            drawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        case WM_USER + 50: {
            int newHover = (lParam != 0) ? static_cast<int>(wParam) : 0;
            if (newHover != m_hoverBtnId) {
                m_hoverBtnId = newHover;
                InvalidateRect(m_btnFps, nullptr, FALSE);
                InvalidateRect(m_btnBoost, nullptr, FALSE);
                InvalidateRect(m_btnVdd, nullptr, FALSE);
                InvalidateRect(m_btnCopy, nullptr, FALSE);
                InvalidateRect(m_btnTray, nullptr, FALSE);
            }
            return 0;
        }
        case WM_APP_REFRESH:
            InvalidateRect(hwnd, nullptr, FALSE);
            if (m_btnFps) InvalidateRect(m_btnFps, nullptr, FALSE);
            if (m_btnBoost) InvalidateRect(m_btnBoost, nullptr, FALSE);
            if (m_btnVdd) InvalidateRect(m_btnVdd, nullptr, FALSE);
            if (m_btnCopy) InvalidateRect(m_btnCopy, nullptr, FALSE);
            if (m_btnTray) InvalidateRect(m_btnTray, nullptr, FALSE);
            return 0;
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == ID_BTN_FPS && m_onToggleUnlimited) {
                m_onToggleUnlimited();
            } else if (id == ID_BTN_BOOST && m_onToggleGameBoost) {
                m_onToggleGameBoost();
            } else if (id == ID_BTN_VDD && m_onInstallVdd) {
                m_onInstallVdd();
            } else if (id == ID_BTN_COPY) {
                copyPrimaryAddress();
            } else if (id == ID_BTN_TRAY) {
                hideToTray();
            }
            return 0;
        }
        case WM_TIMER:
            if (wParam == TIMER_COPY_RESET) {
                KillTimer(hwnd, TIMER_COPY_RESET);
                m_copiedFlash = false;
                if (m_btnCopy) InvalidateRect(m_btnCopy, nullptr, FALSE);
            }
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
            layoutChildren();
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
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
