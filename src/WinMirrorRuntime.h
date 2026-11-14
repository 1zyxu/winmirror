#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>

#ifdef WINMIRROR_RT_EXPORTS
#define WM_RT_API extern "C" __declspec(dllexport)
#else
#define WM_RT_API extern "C"
#endif

#pragma pack(push, 8)
struct WmPreflightReport {
    uint32_t structSize;
    uint32_t monitorRefreshHz;
    uint32_t monitorMaxHz;
    uint32_t monitorWidth;
    uint32_t monitorHeight;
    uint32_t timerResolution100ns;
    uint8_t  isAdmin;
    uint8_t  hasNvencDriver;
    uint8_t  hasVddDriver;
    uint8_t  hasHdrDisplay;
    uint8_t  gpuPrefApplied;
    uint8_t  firewallOk;
    uint8_t  mmcssAvailable;
    uint8_t  reserved;
    char     runtimeVersion[32];
    char     primaryMonitorDesc[128];
    char     vddDeviceDesc[128];
};
#pragma pack(pop)

WM_RT_API const char* WM_GetRuntimeVersion();
WM_RT_API uint32_t    WM_EnableHighResTimer();
WM_RT_API void        WM_DisableHighResTimer();
WM_RT_API HANDLE      WM_EnterMmcssTask(const wchar_t* taskName);
WM_RT_API void        WM_LeaveMmcssTask(HANDLE taskHandle);
WM_RT_API int         WM_RunPreflightAndInstall(const wchar_t* exePath, int applyTweaks, WmPreflightReport* outReport);
WM_RT_API int         WM_InstallVirtualDisplayFromDir(const wchar_t* vddExtractDir);

#ifndef WINMIRROR_RT_EXPORTS
// Header-only dynamic loader so winmirror.exe starts cleanly even if winmirror_rt.dll
// must be downloaded/repaired by Updater before pipeline initialization.
class WinMirrorRuntimeApi {
public:
    using PFN_GetVersion   = const char* (*)();
    using PFN_EnableTimer  = uint32_t (*)();
    using PFN_DisableTimer = void (*)();
    using PFN_EnterMmcss   = HANDLE (*)(const wchar_t*);
    using PFN_LeaveMmcss   = void (*)(HANDLE);
    using PFN_Preflight    = int (*)(const wchar_t*, int, WmPreflightReport*);
    using PFN_InstallVdd   = int (*)(const wchar_t*);

    static WinMirrorRuntimeApi& instance() {
        static WinMirrorRuntimeApi s_inst;
        return s_inst;
    }

    static bool probeDllVersion(const std::wstring& dllPath, std::string& outVer) {
        outVer.clear();
        HMODULE mod = LoadLibraryW(dllPath.c_str());
        if (!mod) return false;
        auto fn = reinterpret_cast<PFN_GetVersion>(GetProcAddress(mod, "WM_GetRuntimeVersion"));
        if (fn) {
            const char* v = fn();
            if (v) outVer = v;
        }
        FreeLibrary(mod);
        return !outVer.empty();
    }

    bool load(const std::wstring& dllPath) {
        if (m_module) return true;
        m_module = LoadLibraryW(dllPath.c_str());
        if (!m_module) return false;
        m_getVersion   = reinterpret_cast<PFN_GetVersion>(GetProcAddress(m_module, "WM_GetRuntimeVersion"));
        m_enableTimer  = reinterpret_cast<PFN_EnableTimer>(GetProcAddress(m_module, "WM_EnableHighResTimer"));
        m_disableTimer = reinterpret_cast<PFN_DisableTimer>(GetProcAddress(m_module, "WM_DisableHighResTimer"));
        m_enterMmcss   = reinterpret_cast<PFN_EnterMmcss>(GetProcAddress(m_module, "WM_EnterMmcssTask"));
        m_leaveMmcss   = reinterpret_cast<PFN_LeaveMmcss>(GetProcAddress(m_module, "WM_LeaveMmcssTask"));
        m_preflight    = reinterpret_cast<PFN_Preflight>(GetProcAddress(m_module, "WM_RunPreflightAndInstall"));
        m_installVdd   = reinterpret_cast<PFN_InstallVdd>(GetProcAddress(m_module, "WM_InstallVirtualDisplayFromDir"));
        if (!m_getVersion || !m_preflight) {
            unload();
            return false;
        }
        return true;
    }

    void unload() {
        if (m_module) {
            FreeLibrary(m_module);
            m_module = nullptr;
        }
        m_getVersion = nullptr;
        m_enableTimer = nullptr;
        m_disableTimer = nullptr;
        m_enterMmcss = nullptr;
        m_leaveMmcss = nullptr;
        m_preflight = nullptr;
        m_installVdd = nullptr;
    }

    bool isLoaded() const { return m_module != nullptr; }

    const char* version() const { return m_getVersion ? m_getVersion() : ""; }

    uint32_t enableHighResTimer() const {
        return m_enableTimer ? m_enableTimer() : 0;
    }

    void disableHighResTimer() const {
        if (m_disableTimer) m_disableTimer();
    }

    HANDLE enterMmcss(const wchar_t* taskName) const {
        if (m_enterMmcss) return m_enterMmcss(taskName);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        return nullptr;
    }

    void leaveMmcss(HANDLE h) const {
        if (m_leaveMmcss && h) m_leaveMmcss(h);
    }

    bool runPreflight(const wchar_t* exePath, bool applyTweaks, WmPreflightReport& report) const {
        std::memset(&report, 0, sizeof(report));
        report.structSize = sizeof(report);
        if (!m_preflight) return false;
        return m_preflight(exePath, applyTweaks ? 1 : 0, &report) != 0;
    }

    bool installVirtualDisplayFromDir(const wchar_t* vddExtractDir) const {
        return m_installVdd && m_installVdd(vddExtractDir) != 0;
    }

private:
    WinMirrorRuntimeApi() = default;
    HMODULE m_module = nullptr;
    PFN_GetVersion   m_getVersion = nullptr;
    PFN_EnableTimer  m_enableTimer = nullptr;
    PFN_DisableTimer m_disableTimer = nullptr;
    PFN_EnterMmcss   m_enterMmcss = nullptr;
    PFN_LeaveMmcss   m_leaveMmcss = nullptr;
    PFN_Preflight    m_preflight = nullptr;
    PFN_InstallVdd   m_installVdd = nullptr;
};
#endif
