#pragma once

#include "Common.h"
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct WinMirrorStatus {
    bool streamingReady = false;
    bool isAdmin = false;
    bool hasVdd = false;
    bool installingVdd = false;
    bool unlimitedFps = false;
    bool gameBoost = true;
    bool ecoQosOff = false;
    bool firewallOk = false;
    bool hdrActive = false;
    bool isHevc = false;
    bool dllLoaded = false;
    bool audioActive = false;
    bool btReady = false;
    bool btConnected = false;
    int clients = 0;
    int bitrateScalePct = 100;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t monitorHz = 60;
    uint32_t targetFps = 60;
    uint32_t targetMbps = 0;
    uint32_t audioRate = 48000;
    uint32_t keyframesSent = 0;
    uint32_t captureSkips = 0;
    float actualFps = 0.0f;
    float actualMbps = 0.0f;
    float encodeAvgMs = 0.0f;
    float avgFrameKb = 0.0f;
    std::string gpuMode = "INIT";
    std::string encoderName;
    std::string captureName;
    std::string pixelFormat;
    std::string audioName;
    std::string btStatus = "Initializing";
    std::string endpointsSummary;
    std::string primaryConnectAddr;
    std::string connectedHost;
    std::vector<AirPlayTarget> receivers;
    std::string dllVersion;
    std::string statusBanner = "Searching for AirPlay receivers...";
};

class WinMirrorGui {
public:
    WinMirrorGui();
    ~WinMirrorGui();

    void setActions(std::function<void()> onQuit,
                    std::function<void(const AirPlayTarget&)> onSelectTarget = nullptr,
                    std::function<void(const std::string&)> onConnectIp = nullptr,
                    std::function<void()> onRefresh = nullptr,
                    std::function<void()> onCheckUpdate = nullptr);

    bool createAndShow();
    void updateStatus(const WinMirrorStatus& st);
    void setStatusBanner(const std::string& msg);
    int runMessageLoop();
    void requestClose();
    void removeTrayIcon();

private:
    static LRESULT CALLBACK wndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void paintWindow(HWND hwnd);
    void ensureCanvas(HDC refDc, int w, int h);
    void ensureTextScratch(HDC refDc, int w, int h);
    void releaseBitmaps();
    void drawTextGlass(const RECT& r, HFONT font, struct GlassColor col, const std::wstring& txt, UINT fmt);
    void rebuildFonts();
    int hitTestButton(POINT pt) const;
    int hitTestReceiverRow(POINT pt, size_t maxRows) const;
    void invokeButtonAction(int btnId);
    void appendIpCharsFromClipboard();
    void addTrayIcon();
    void showFromTray();
    void showTrayContextMenu();
    void copyPrimaryAddress();

    HWND m_hwnd = nullptr;
    HICON m_appIcon = nullptr;
    HFONT m_fontTitle = nullptr;
    HFONT m_fontRow = nullptr;
    HFONT m_fontLabel = nullptr;
    HFONT m_fontBody = nullptr;
    HFONT m_fontSmall = nullptr;

    HDC m_canvasDc = nullptr;
    HBITMAP m_canvasBmp = nullptr;
    HGDIOBJ m_canvasOldBmp = nullptr;
    uint32_t* m_canvasPx = nullptr;
    int m_canvasW = 0;
    int m_canvasH = 0;

    HDC m_scratchDc = nullptr;
    HBITMAP m_scratchBmp = nullptr;
    HGDIOBJ m_scratchOldBmp = nullptr;
    uint32_t* m_scratchPx = nullptr;
    int m_scratchW = 0;
    int m_scratchH = 0;

    UINT m_dpi = 96;
    bool m_glassBlurActive = false;
    bool m_trayAdded = false;
    bool m_ipFocused = false;
    bool m_caretOn = true;
    int m_hoverBtnId = 0;
    int m_pressedBtnId = 0;
    int m_hoverRowIdx = -1;
    std::string m_manualIp;

    std::mutex m_mtx;
    WinMirrorStatus m_status;

    std::function<void()> m_onQuit;
    std::function<void(const AirPlayTarget&)> m_onSelectTarget;
    std::function<void(const std::string&)> m_onConnectIp;
    std::function<void()> m_onRefresh;
    std::function<void()> m_onCheckUpdate;
};
