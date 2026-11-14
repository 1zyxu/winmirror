#define WINMIRROR_RT_EXPORTS
#include "Common.h"
#include "WinMirrorRuntime.h"

#include <mmsystem.h>
#include <atomic>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>

namespace {

std::atomic<bool> g_timerActive{false};

void copyBounded(char* dst, size_t dstCap, const char* src) {
    if (!dst || dstCap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    std::snprintf(dst, dstCap, "%s", src);
}

void wideToUtf8(const wchar_t* src, char* dst, size_t dstCap) {
    if (!dst || dstCap == 0) return;
    dst[0] = '\0';
    if (!src || !src[0]) return;
    WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, static_cast<int>(dstCap), nullptr, nullptr);
}

bool containsCaseInsensitive(const wchar_t* haystack, const wchar_t* needle) {
    if (!haystack || !needle || !needle[0]) return false;
    std::wstring h(haystack), n(needle);
    for (wchar_t& c : h) c = towlower(c);
    for (wchar_t& c : n) c = towlower(c);
    return h.find(n) != std::wstring::npos;
}

bool checkAdminToken() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin == TRUE;
}

bool applyDirectXHighPerfGpuPref(const wchar_t* exePath) {
    if (!exePath || !exePath[0]) return false;
    HKEY hKey = nullptr;
    LONG rc = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\DirectX\\UserGpuPreferences",
        0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_READ | KEY_WRITE, nullptr, &hKey, nullptr);
    if (rc != ERROR_SUCCESS || !hKey) return false;

    const wchar_t* desired = L"GpuPreference=2;";
    wchar_t existing[64] = {};
    DWORD type = 0;
    DWORD byteLen = sizeof(existing);
    bool alreadyOk = (RegQueryValueExW(hKey, exePath, nullptr, &type,
                                       reinterpret_cast<LPBYTE>(existing), &byteLen) == ERROR_SUCCESS &&
                      type == REG_SZ && _wcsicmp(existing, desired) == 0);
    bool ok = alreadyOk;
    if (!alreadyOk) {
        DWORD writeBytes = static_cast<DWORD>((std::wcslen(desired) + 1) * sizeof(wchar_t));
        ok = (RegSetValueExW(hKey, exePath, 0, REG_SZ,
                              reinterpret_cast<const BYTE*>(desired), writeBytes) == ERROR_SUCCESS);
    }
    RegCloseKey(hKey);
    return ok;
}

bool runHiddenProcess(const std::wstring& cmdLine, DWORD timeoutMs, DWORD& outExitCode) {
    outExitCode = 1;
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, timeoutMs);
    GetExitCodeProcess(pi.hProcess, &outExitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

bool ensureFirewallRules(const wchar_t* exePath, bool isAdmin, bool applyTweaks) {
    HKEY hFw = nullptr;
    bool hasTcpRule = false;
    if (RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy\\FirewallRules",
            0, KEY_READ, &hFw) == ERROR_SUCCESS && hFw) {
        DWORD idx = 0;
        wchar_t valName[512] = {};
        wchar_t valData[1024] = {};
        for (;;) {
            DWORD nameCap = 512;
            DWORD dataCap = sizeof(valData) - sizeof(wchar_t);
            DWORD type = 0;
            LONG r = RegEnumValueW(hFw, idx++, valName, &nameCap, nullptr, &type,
                                   reinterpret_cast<LPBYTE>(valData), &dataCap);
            if (r == ERROR_NO_MORE_ITEMS) break;
            if (r == ERROR_SUCCESS && type == REG_SZ) {
                valData[1023] = L'\0';
                if (containsCaseInsensitive(valData, L"Name=WinMirror-AirPlay-TCP")) {
                    hasTcpRule = true;
                    break;
                }
            }
        }
        RegCloseKey(hFw);
    }

    if (hasTcpRule) return true;
    if (!isAdmin || !applyTweaks || !exePath || !exePath[0]) return false;

    DWORD exitTcp = 1, exitUdp = 1;
    std::wstring tcpCmd = std::wstring(L"netsh advfirewall firewall add rule name=\"WinMirror-AirPlay-TCP\" ") +
        L"dir=in action=allow program=\"" + exePath + L"\" enable=yes protocol=TCP localport=7001";
    std::wstring udpCmd = std::wstring(L"netsh advfirewall firewall add rule name=\"WinMirror-AirPlay-UDP\" ") +
        L"dir=in action=allow program=\"" + exePath + L"\" enable=yes protocol=UDP localport=8889";
    runHiddenProcess(tcpCmd, 8000, exitTcp);
    runHiddenProcess(udpCmd, 8000, exitUdp);
    return exitTcp == 0;
}

bool checkNvencDriver() {
    HMODULE nv = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!nv) nv = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (nv) {
        FreeLibrary(nv);
        return true;
    }
    return false;
}

bool checkMmcssSupport() {
    HMODULE avrt = LoadLibraryW(L"avrt.dll");
    if (!avrt) return false;
    bool ok = GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW") != nullptr &&
              GetProcAddress(avrt, "AvRevertMmThreadCharacteristics") != nullptr;
    FreeLibrary(avrt);
    return ok;
}

void inspectDisplays(WmPreflightReport& rep) {
    DISPLAY_DEVICEW dd = {};
    dd.cb = sizeof(dd);
    std::wstring primaryDevName;

    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if ((dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0) continue;

        bool isVirtual = containsCaseInsensitive(dd.DeviceString, L"Virtual") ||
                         containsCaseInsensitive(dd.DeviceString, L"MttVDD") ||
                         containsCaseInsensitive(dd.DeviceString, L"IddSample") ||
                         containsCaseInsensitive(dd.DeviceString, L"Parsec");
        if (isVirtual && !rep.hasVddDriver) {
            rep.hasVddDriver = 1;
            wideToUtf8(dd.DeviceString, rep.vddDeviceDesc, sizeof(rep.vddDeviceDesc));
        }

        if ((dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) || primaryDevName.empty()) {
            primaryDevName = dd.DeviceName;
            wideToUtf8(dd.DeviceString, rep.primaryMonitorDesc, sizeof(rep.primaryMonitorDesc));
        }
    }

    const wchar_t* queryDev = primaryDevName.empty() ? nullptr : primaryDevName.c_str();
    DEVMODEW cur = {};
    cur.dmSize = sizeof(cur);
    if (EnumDisplaySettingsExW(queryDev, ENUM_CURRENT_SETTINGS, &cur, 0)) {
        rep.monitorWidth = cur.dmPelsWidth;
        rep.monitorHeight = cur.dmPelsHeight;
        rep.monitorRefreshHz = cur.dmDisplayFrequency > 1 ? cur.dmDisplayFrequency : 60;
        rep.monitorMaxHz = rep.monitorRefreshHz;
    }

    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    for (DWORD m = 0; EnumDisplaySettingsExW(queryDev, m, &mode, 0); ++m) {
        if (rep.monitorWidth > 0 &&
            (mode.dmPelsWidth != rep.monitorWidth || mode.dmPelsHeight != rep.monitorHeight)) {
            continue;
        }
        if (mode.dmDisplayFrequency > rep.monitorMaxHz && mode.dmDisplayFrequency <= 1000) {
            rep.monitorMaxHz = mode.dmDisplayFrequency;
        }
    }

    // Also check PnP Display devices in case Virtual Display Driver is installed but toggled off
    if (!rep.hasVddDriver) {
        HMODULE sysInf = nullptr;
        (void)sysInf;
        DISPLAY_DEVICEW anyDev = {};
        anyDev.cb = sizeof(anyDev);
        for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &anyDev, 0); ++i) {
            if (containsCaseInsensitive(anyDev.DeviceString, L"MttVDD") ||
                containsCaseInsensitive(anyDev.DeviceString, L"Virtual Display") ||
                containsCaseInsensitive(anyDev.DeviceString, L"Parsec Virtual")) {
                rep.hasVddDriver = 1;
                wideToUtf8(anyDev.DeviceString, rep.vddDeviceDesc, sizeof(rep.vddDeviceDesc));
                break;
            }
        }
    }
}

bool fileExistsW(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void enableHdrInVddXml(const std::wstring& xmlPath) {
    if (!fileExistsW(xmlPath)) return;
    HANDLE hFile = CreateFileW(xmlPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;

    DWORD sz = GetFileSize(hFile, nullptr);
    if (sz == INVALID_FILE_SIZE || sz == 0 || sz > 1024 * 1024) {
        CloseHandle(hFile);
        return;
    }
    std::string content(sz, '\0');
    DWORD readBytes = 0;
    if (ReadFile(hFile, &content[0], sz, &readBytes, nullptr) && readBytes > 0) {
        content.resize(readBytes);
        bool changed = false;
        size_t hdrPos = content.find("<HDRPlus>false</HDRPlus>");
        if (hdrPos != std::string::npos) {
            content.replace(hdrPos, 24, "<HDRPlus>true</HDRPlus>");
            changed = true;
        }
        size_t sdr10Pos = content.find("<SDR10bit>false</SDR10bit>");
        if (sdr10Pos != std::string::npos) {
            content.replace(sdr10Pos, 26, "<SDR10bit>true</SDR10bit>");
            changed = true;
        }
        if (content.find("3840") == std::string::npos) {
            size_t resEnd = content.find("</resolutions>");
            if (resEnd != std::string::npos) {
                content.insert(resEnd,
                    "    <resolution><width>3840</width><height>2160</height><refresh_rate>60</refresh_rate></resolution>\n"
                    "    <resolution><width>3840</width><height>2160</height><refresh_rate>120</refresh_rate></resolution>\n"
                    "    <resolution><width>3840</width><height>2160</height><refresh_rate>240</refresh_rate></resolution>\n");
                changed = true;
            }
        }
        if (changed) {
            SetFilePointer(hFile, 0, nullptr, FILE_BEGIN);
            DWORD written = 0;
            WriteFile(hFile, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
            SetEndOfFile(hFile);
        }
    }
    CloseHandle(hFile);
}

} // namespace

WM_RT_API const char* WM_GetRuntimeVersion() {
    return WINMIRROR_VERSION;
}

WM_RT_API uint32_t WM_EnableHighResTimer() {
    timeBeginPeriod(1);
    g_timerActive.store(true);

    ULONG actual100ns = 10000;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        using PFN_NtSetTimerResolution = LONG (WINAPI*)(ULONG, BOOLEAN, PULONG);
        auto pNtSetTimerResolution = reinterpret_cast<PFN_NtSetTimerResolution>(
            GetProcAddress(ntdll, "NtSetTimerResolution"));
        if (pNtSetTimerResolution) {
            pNtSetTimerResolution(5000, TRUE, &actual100ns); // 0.5 ms kernel timer resolution
        }
    }
    return static_cast<uint32_t>(actual100ns);
}

WM_RT_API void WM_DisableHighResTimer() {
    if (!g_timerActive.exchange(false)) return;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        using PFN_NtSetTimerResolution = LONG (WINAPI*)(ULONG, BOOLEAN, PULONG);
        auto pNtSetTimerResolution = reinterpret_cast<PFN_NtSetTimerResolution>(
            GetProcAddress(ntdll, "NtSetTimerResolution"));
        if (pNtSetTimerResolution) {
            ULONG unused = 0;
            pNtSetTimerResolution(0, FALSE, &unused);
        }
    }
    timeEndPeriod(1);
}

WM_RT_API HANDLE WM_EnterMmcssTask(const wchar_t* taskName) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HMODULE avrt = LoadLibraryW(L"avrt.dll");
    if (!avrt) return nullptr;

    using PFN_AvSetMmThreadCharacteristicsW = HANDLE (WINAPI*)(LPCWSTR, LPDWORD);
    using PFN_AvSetMmThreadPriority = BOOL (WINAPI*)(HANDLE, int);
    auto pSetChar = reinterpret_cast<PFN_AvSetMmThreadCharacteristicsW>(
        GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW"));
    auto pSetPrio = reinterpret_cast<PFN_AvSetMmThreadPriority>(
        GetProcAddress(avrt, "AvSetMmThreadPriority"));
    if (!pSetChar) return nullptr;

    DWORD taskIdx = 0;
    HANDLE hTask = pSetChar(taskName ? taskName : L"Games", &taskIdx);
    if (hTask && pSetPrio) {
        pSetPrio(hTask, 2 /* AVRT_PRIORITY_CRITICAL */);
    }
    return hTask;
}

WM_RT_API void WM_LeaveMmcssTask(HANDLE taskHandle) {
    if (!taskHandle) return;
    HMODULE avrt = GetModuleHandleW(L"avrt.dll");
    if (!avrt) return;
    using PFN_AvRevertMmThreadCharacteristics = BOOL (WINAPI*)(HANDLE);
    auto pRevert = reinterpret_cast<PFN_AvRevertMmThreadCharacteristics>(
        GetProcAddress(avrt, "AvRevertMmThreadCharacteristics"));
    if (pRevert) pRevert(taskHandle);
}

WM_RT_API int WM_RunPreflightAndInstall(const wchar_t* exePath, int applyTweaks, WmPreflightReport* outReport) {
    if (!outReport || outReport->structSize < sizeof(WmPreflightReport)) return 0;
    uint32_t sz = outReport->structSize;
    std::memset(outReport, 0, sz);
    outReport->structSize = sz;

    copyBounded(outReport->runtimeVersion, sizeof(outReport->runtimeVersion), WINMIRROR_VERSION);
    bool isAdmin = checkAdminToken();
    outReport->isAdmin = isAdmin ? 1 : 0;
    outReport->hasNvencDriver = checkNvencDriver() ? 1 : 0;
    outReport->mmcssAvailable = checkMmcssSupport() ? 1 : 0;

    if (applyTweaks) {
        outReport->gpuPrefApplied = applyDirectXHighPerfGpuPref(exePath) ? 1 : 0;
        outReport->timerResolution100ns = WM_EnableHighResTimer();
        enableHdrInVddXml(L"C:\\VirtualDisplayDriver\\vdd_settings.xml");
    } else {
        outReport->timerResolution100ns = 10000;
    }

    outReport->firewallOk = ensureFirewallRules(exePath, isAdmin, applyTweaks != 0) ? 1 : 0;
    inspectDisplays(*outReport);
    return 1;
}

WM_RT_API int WM_InstallVirtualDisplayFromDir(const wchar_t* vddExtractDir) {
    if (!vddExtractDir || !vddExtractDir[0]) return 0;
    std::wstring base(vddExtractDir);

    std::wstring infCandidates[] = {
        base + L"\\SignedDrivers\\x86\\VDD\\MttVDD.inf",
        base + L"\\VirtualDisplayDriver\\MttVDD.inf",
        base + L"\\MttVDD.inf"
    };
    std::wstring infPath;
    for (const auto& c : infCandidates) {
        if (fileExistsW(c)) {
            infPath = c;
            break;
        }
    }
    if (infPath.empty()) return 0;

    std::wstring xmlCandidates[] = {
        base + L"\\Dependencies\\vdd_settings.xml",
        base + L"\\SignedDrivers\\x86\\VDD\\vdd_settings.xml",
        base + L"\\VirtualDisplayDriver\\vdd_settings.xml"
    };
    for (const auto& xp : xmlCandidates) {
        if (fileExistsW(xp)) {
            enableHdrInVddXml(xp);
            CreateDirectoryW(L"C:\\VirtualDisplayDriver", nullptr);
            CopyFileW(xp.c_str(), L"C:\\VirtualDisplayDriver\\vdd_settings.xml", FALSE);
            break;
        }
    }

    DWORD exitCode = 1;
    std::wstring pnpCmd = L"pnputil.exe /add-driver \"" + infPath + L"\" /install";
    if (!runHiddenProcess(pnpCmd, 45000, exitCode)) return 0;

    std::wstring devconPath = base + L"\\Dependencies\\devcon.exe";
    WmPreflightReport rep = {};
    rep.structSize = sizeof(rep);
    inspectDisplays(rep);
    if (!rep.hasVddDriver && fileExistsW(devconPath)) {
        DWORD devExit = 1;
        std::wstring devCmd = L"\"" + devconPath + L"\" install \"" + infPath + L"\" Root\\MttVDD";
        runHiddenProcess(devCmd, 45000, devExit);
        if (devExit == 0) exitCode = 0;
    }

    return (exitCode == 0 || exitCode == ERROR_SUCCESS_REBOOT_REQUIRED) ? 1 : 0;
}
