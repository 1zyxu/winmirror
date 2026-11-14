#pragma once

#include "Common.h"
#include <functional>
#include <mutex>
#include <string>

struct WinMirrorStatus {
    bool streamingReady = false;
    bool isAdmin = false;
    bool hasVdd = false;
    bool installingVdd = false;
    bool unlimitedFps = false;
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
    float actualFps = 0.0f;
    float actualMbps = 0.0f;
    std::string gpuMode = "INIT";
    std::string encoderName;
    std::string captureName;
    std::string pixelFormat;
    std::string audioName;
    std::string btStatus = "Initializing";
    std::string endpointsSummary;
    std::string primaryConnectAddr;
    std::string dllVersion;
    std::string statusBanner = "Starting WinMirror AirPlay engine...";
};

class WinMirrorGui {
public:
    WinMirrorGui();
    ~WinMirrorGui();

    void setActions(std::function<void()> onToggleUnlimitedFps,
                    std::function<void()> onInstallVdd,
                    std::function<void()> onQuit);

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
    void drawButton(const DRAWITEMSTRUCT* dis);
    void layoutChildren();
    void rebuildFonts();
    void addTrayIcon();
    void showFromTray();
    void hideToTray();
    void showTrayContextMenu();
    void copyPrimaryAddress();

    HWND m_hwnd = nullptr;
    HWND m_btnFps = nullptr;
    HWND m_btnVdd = nullptr;
    HWND m_btnCopy = nullptr;
    HWND m_btnTray = nullptr;

    HICON m_appIcon = nullptr;
    HFONT m_fontTitle = nullptr;
    HFONT m_fontMetric = nullptr;
    HFONT m_fontLabel = nullptr;
    HFONT m_fontBody = nullptr;
    HFONT m_fontSmall = nullptr;

    UINT m_dpi = 96;
    bool m_trayAdded = false;
    bool m_hiddenToTray = false;
    bool m_copiedFlash = false;
    int m_hoverBtnId = 0;

    std::mutex m_mtx;
    WinMirrorStatus m_status;

    std::function<void()> m_onToggleUnlimited;
    std::function<void()> m_onInstallVdd;
    std::function<void()> m_onQuit;
};
