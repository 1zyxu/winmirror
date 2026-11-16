#include "Common.h"
#include "CapturePolicy.h"
#include "ScreenCapture.h"
#include "Encoder.h"
#include "AirPlayServer.h"
#include "BluetoothBridge.h"
#include "Beacon.h"
#include "Updater.h"
#include "UpdaterLogic.h"
#include "AudioCapture.h"
#include "WinMirrorRuntime.h"
#include "GuiWindow.h"
#include "SecProtect.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <streambuf>
#include <cstdlib>
#include <thread>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <optional>
#include <iphlpapi.h>
#include <mmsystem.h>
#include <objbase.h>
#include <shellapi.h>

// Signal NVIDIA Optimus and AMD Switchable Graphics drivers to expose the discrete GPU
extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

static std::atomic<bool> g_running{true};
static std::atomic<bool> g_unlimitedFpsFlag{false};
static std::atomic<bool> g_gameBoostFlag{true};
static std::atomic<bool> g_ecoQosDisabled{false};
static std::atomic<bool> g_hasVddState{false};
static std::atomic<bool> g_installingVdd{false};
static std::atomic<uint64_t> g_captureSkips{0};
static std::atomic<uint64_t> g_stateChangedSeq{0};
static std::mutex g_actionMtx;
static std::optional<AirPlayTarget> g_pendingTarget;
static std::string g_pendingManualIp;
static bool g_rescanPending = false;
static WinMirrorGui* g_trayCleanupGui = nullptr;

static void applyGameProcessBoost(bool enable) {
    SetPriorityClass(GetCurrentProcess(), enable ? HIGH_PRIORITY_CLASS : ABOVE_NORMAL_PRIORITY_CLASS);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return;
    struct WmPowerThrottlingState {
        ULONG Version;
        ULONG ControlMask;
        ULONG StateMask;
    };
    using PFN_SetProcessInformation = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
    auto pSetProcInfo = reinterpret_cast<PFN_SetProcessInformation>(
        GetProcAddress(k32, "SetProcessInformation"));
    if (pSetProcInfo) {
        WmPowerThrottlingState pts = {};
        pts.Version = 1;
        // EXECUTION_SPEED (0x1) | IGNORE_TIMER_RESOLUTION (0x4): prevents Windows 11 EcoQoS throttling behind fullscreen games
        pts.ControlMask = 0x1 | 0x4;
        pts.StateMask = enable ? 0u : 0x1u;
        if (pSetProcInfo(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &pts, sizeof(pts))) {
            g_ecoQosDisabled.store(enable);
        }
    }
}

class GuiLogStreamBuf : public std::streambuf {
public:
    explicit GuiLogStreamBuf(WinMirrorGui* gui) : m_gui(gui) {}
protected:
    int overflow(int ch) override {
        if (ch == EOF) return 0;
        char c = static_cast<char>(ch);
        std::lock_guard<std::mutex> lock(m_mtx);
        if (c == '\r') {
            m_line.clear();
            return ch;
        }
        if (c == '\n') {
            if (!m_line.empty() && m_line.rfind("[Stream]", 0) != 0 && m_gui) {
                m_gui->setStatusBanner(m_line);
            }
            m_line.clear();
        } else if (m_line.size() < 220) {
            m_line.push_back(c);
        }
        return ch;
    }
private:
    WinMirrorGui* m_gui;
    std::mutex m_mtx;
    std::string m_line;
};

static BOOL WINAPI consoleCtrlHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT) {
        std::cout << "\n\n[WinMirror AirPlay] Stopping..." << std::endl;
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

static bool isRunningAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin == TRUE;
}

static void deleteTempFileBestEffort(const std::wstring& path) {
    if (path.empty()) return;
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (DeleteFileW(path.c_str())) return;
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return;
        if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION) break;
        Sleep(60);
    }
    MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

static void cleanupPreviousUpdateBinary() {
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(NULL, exePath, MAX_PATH) > 0) {
        std::wstring base(exePath);
        deleteTempFileBestEffort(base + L".old.exe");
        deleteTempFileBestEffort(base + L".update.exe");
    }
    std::wstring exeDir = Updater::getExecutableDir();
    std::wstring dllBase = exeDir + L"\\winmirror_rt.dll";
    deleteTempFileBestEffort(dllBase + L".tmp");
    deleteTempFileBestEffort(dllBase + L".update");
    deleteTempFileBestEffort(exeDir + L"\\vdd_driver.zip");

    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW((exeDir + L"\\*").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            int len = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nullptr, 0, nullptr, nullptr);
            if (len <= 1) continue;
            std::string nameUtf8(static_cast<size_t>(len - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, &nameUtf8[0], len, nullptr, nullptr);
            if (updater_logic::isUpdateTempFile(nameUtf8)) {
                deleteTempFileBestEffort(exeDir + L"\\" + fd.cFileName);
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
}

struct ComMtaScope {
    HRESULT hr = E_FAIL;
    ComMtaScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComMtaScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

static uint16_t buildHelloFlags(bool btReady, const ScreenCapture& /*cap*/, const Encoder& enc, bool unlimitedFps) {
    uint16_t f = btReady ? HELLO_FLAG_BT : 0;
    if (enc.isNvidiaEncoder()) f |= HELLO_FLAG_NVIDIA;
    if (enc.isZeroCopyActive()) f |= HELLO_FLAG_ZEROCOPY;
    if (enc.isHardwareGpu()) f |= HELLO_FLAG_HW_GPU;
    if (unlimitedFps) f |= HELLO_FLAG_UNLIMITED_FPS;
    return f;
}

static int parseCliFps(const std::string& val) {
    std::string lower = val;
    for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lower == "auto") return 0;
    if (lower == "unlimited" || lower == "max" || lower == "uncapped") return -1;
    return std::stoi(val);
}

static void printPreflightReport(const WmPreflightReport& rep, bool dllLoaded) {
    double timerMs = rep.timerResolution100ns > 0 ? (rep.timerResolution100ns / 10000.0) : 1.0;
    std::cout << "[Preflight] Runtime DLL : "
              << (dllLoaded ? (std::string("winmirror_rt.dll v") + rep.runtimeVersion) : "Fallback (DLL unavailable)")
              << " | Timer " << std::fixed << std::setprecision(2) << timerMs << " ms"
              << " | MMCSS " << (rep.mmcssAvailable ? "Ready" : "Std") << std::endl;
    std::cout << "[Preflight] GPU / Net   : "
              << (rep.hasNvencDriver ? "NVIDIA NVENC Driver Ready" : "No NVENC DLL (iGPU/SW)")
              << " | DX High-Perf " << (rep.gpuPrefApplied ? "Installed" : "Pending")
              << " | Firewall " << (rep.firewallOk ? "Allowed (TCP 7001/UDP 8889)" : "Run as Admin") << std::endl;
    std::cout << "[Preflight] Display     : "
              << (rep.primaryMonitorDesc[0] ? rep.primaryMonitorDesc : "Primary Monitor")
              << " (" << rep.monitorWidth << "x" << rep.monitorHeight << " @ "
              << (rep.monitorRefreshHz ? rep.monitorRefreshHz : 60) << " Hz, Max "
              << (rep.monitorMaxHz ? rep.monitorMaxHz : 60) << " Hz)" << std::endl;
    std::cout << "[Preflight] Virtual HDR : "
              << (rep.hasVddDriver
                    ? (std::string("Installed (") + (rep.vddDeviceDesc[0] ? rep.vddDeviceDesc : "Virtual Display") + ")")
                    : "Not installed (run winmirror.exe --install-vdd for 4K HDR 120/240Hz virtual monitor)")
              << std::endl;
}

static bool installVirtualDisplayDriver(const std::wstring& exeDir) {
    std::wstring vddDir = exeDir + L"\\vdd";
    std::wstring zipPath = exeDir + L"\\vdd_driver.zip";
    if (!Updater::downloadVirtualDisplayPackage(zipPath)) {
        std::cerr << "[Driver] Failed to download Virtual Display Driver package." << std::endl;
        return false;
    }

    CreateDirectoryW(vddDir.c_str(), nullptr);
    std::wstring tarCmd = L"tar.exe -xf \"" + zipPath + L"\" -C \"" + vddDir + L"\"";
    std::vector<wchar_t> cmdBuf(tarCmd.begin(), tarCmd.end());
    cmdBuf.push_back(L'\0');
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        DeleteFileW(zipPath.c_str());
        std::cerr << "[Driver] Failed to launch tar.exe to extract Virtual Display Driver." << std::endl;
        return false;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD tarExit = 1;
    GetExitCodeProcess(pi.hProcess, &tarExit);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    DeleteFileW(zipPath.c_str());
    if (tarExit != 0) {
        std::cerr << "[Driver] Failed to extract Virtual Display Driver archive." << std::endl;
        return false;
    }

    bool ok = WinMirrorRuntimeApi::instance().installVirtualDisplayFromDir(vddDir.c_str());
    if (ok) {
        std::cout << "[Driver] Virtual 4K/HDR Display Driver installed! Enable HDR / resolution in Windows Display Settings." << std::endl;
    } else {
        std::cerr << "[Driver] Virtual Display Driver install did not complete cleanly. Check vdd\\ folder or run as Administrator." << std::endl;
    }
    return ok;
}

static const char* gpuModeTag(uint16_t flags) {
    bool nv = (flags & HELLO_FLAG_NVIDIA) != 0;
    bool zc = (flags & HELLO_FLAG_ZEROCOPY) != 0;
    bool hw = (flags & HELLO_FLAG_HW_GPU) != 0;
    if (nv && zc) return "NVENC-ZC";
    if (nv) return "NVENC";
    if (hw && zc) return "GPU-ZC";
    if (hw) return "GPU";
    return "CPU";
}

static bool asciiContainsIC(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return false;
    std::string h(hay), n(needle);
    for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h.find(n) != std::string::npos;
}

static std::string collectLocalIpsCsv(int port, bool printBanner,
                                      std::string* outSummary = nullptr,
                                      std::string* outPrimaryAddr = nullptr) {
    if (printBanner) {
        std::cout << "==================================================================" << std::endl;
        std::cout << "  WINMIRROR AirPlay v" << WINMIRROR_VERSION << " -- GPU HEVC/H264 HDR + Bluetooth" << std::endl;
        std::cout << "==================================================================" << std::endl;
        std::cout << "AirPlay Discovery Transports (USB-C / Router LAN / Wi-Fi / BT):" << std::endl;
    }

    struct NetEndpoint {
        int priority;
        std::string ip;
        const char* tag;
        std::string desc;
    };
    std::vector<NetEndpoint> endpoints;

    ULONG bufLen = sizeof(IP_ADAPTER_INFO);
    std::vector<BYTE> buf(bufLen);
    PIP_ADAPTER_INFO pAdapter = reinterpret_cast<PIP_ADAPTER_INFO>(buf.data());

    if (GetAdaptersInfo(pAdapter, &bufLen) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
        pAdapter = reinterpret_cast<PIP_ADAPTER_INFO>(buf.data());
    }

    if (GetAdaptersInfo(pAdapter, &bufLen) == NO_ERROR) {
        while (pAdapter) {
            std::string ip = pAdapter->IpAddressList.IpAddress.String;
            if (ip != "0.0.0.0" && ip != "127.0.0.1") {
                bool dup = false;
                for (const auto& existing : endpoints) {
                    if (existing.ip == ip) { dup = true; break; }
                }
                if (!dup) {
                    const char* d = pAdapter->Description;
                    bool isVirtual = asciiContainsIC(d, "vEthernet") || asciiContainsIC(d, "Hyper-V") ||
                                     asciiContainsIC(d, "VMware") || asciiContainsIC(d, "VirtualBox") ||
                                     asciiContainsIC(d, "Wintun") || asciiContainsIC(d, "Tailscale") ||
                                     asciiContainsIC(d, "ZeroTier") || asciiContainsIC(d, "TAP-");
                    bool isUsb = (ip.rfind("169.254.", 0) == 0) || asciiContainsIC(d, "USB") ||
                                 asciiContainsIC(d, "RNDIS") || asciiContainsIC(d, "NCM") ||
                                 asciiContainsIC(d, "Remote NDIS") || asciiContainsIC(d, "Apple");
                    bool isWifi = (pAdapter->Type == 71 /* IF_TYPE_IEEE80211 */) ||
                                  asciiContainsIC(d, "Wi-Fi") || asciiContainsIC(d, "Wireless") ||
                                  asciiContainsIC(d, "802.11");
                    int prio = 3;
                    const char* tag = "NET";
                    if (isVirtual) {
                        prio = 4; tag = "Virtual/VPN";
                    } else if (isUsb) {
                        prio = 0; tag = "USB";
                    } else if (pAdapter->Type == MIB_IF_TYPE_ETHERNET && !isWifi) {
                        prio = 1; tag = "LAN";
                    } else if (isWifi) {
                        prio = 2; tag = "Wi-Fi";
                    }
                    endpoints.push_back({prio, ip, tag, d ? d : ""});
                }
            }
            pAdapter = pAdapter->Next;
        }
    }

    std::stable_sort(endpoints.begin(), endpoints.end(),
        [](const NetEndpoint& a, const NetEndpoint& b) { return a.priority < b.priority; });

    std::ostringstream ipsCsv;
    std::ostringstream summary;
    std::string primaryAddr;
    int shownSummary = 0;
    for (size_t i = 0; i < endpoints.size(); ++i) {
        if (printBanner) {
            std::cout << "  -> [" << endpoints[i].tag << "] airplay://" << endpoints[i].ip << ":" << port
                      << " (" << endpoints[i].desc << ")" << std::endl;
        }
        if (i > 0) ipsCsv << ",";
        ipsCsv << endpoints[i].ip;
        if (endpoints[i].priority < 4) {
            if (primaryAddr.empty()) {
                primaryAddr = endpoints[i].ip + ":" + std::to_string(port);
            }
            if (shownSummary < 3) {
                if (shownSummary > 0) summary << "  |  ";
                summary << endpoints[i].tag << " " << endpoints[i].ip << ":" << port;
                shownSummary++;
            }
        }
    }
    if (primaryAddr.empty() && !endpoints.empty()) {
        primaryAddr = endpoints[0].ip + ":" + std::to_string(port);
    }
    if (outSummary) {
        *outSummary = summary.str().empty() ? primaryAddr : summary.str();
    }
    if (outPrimaryAddr) {
        *outPrimaryAddr = primaryAddr;
    }
    if (printBanner) {
        std::cout << "==================================================================\n" << std::endl;
    }
    return ipsCsv.str();
}

static int runWinMirrorEngine(const std::vector<std::string>& args, WinMirrorGui* gui, bool skipStartupUpdate = false) {
    ComMtaScope comScope;
    wm_sec::hideCurrentThread();
    cleanupPreviousUpdateBinary();
    bool isAdmin = isRunningAsAdmin();
    if (!isAdmin) {
        std::cout << "[WinMirror] Note: Running without Administrator rights (some elevated apps may not mirror)." << std::endl;
    }

    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
    bool gameBoost = g_gameBoostFlag.load();
    applyGameProcessBoost(gameBoost);

    int port = AIRPLAY_PORT;
    int cliFps = 0; // 0 = auto native monitor refresh rate, -1 = unlimited event-driven FPS
    bool checkUpdate = true;
    bool checkOnly = false;
    bool installOnly = false;
    bool installVdd = false;
    bool updateOnly = false;

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--port" && i + 1 < args.size()) {
            port = std::stoi(args[++i]);
        } else if (arg == "--fps" && i + 1 < args.size()) {
            cliFps = parseCliFps(args[++i]);
        } else if (arg == "--update") {
            checkUpdate = true;
        } else if (arg == "--no-update") {
            checkUpdate = false;
        } else if (arg == "--update-only") {
            updateOnly = true;
        } else if (arg == "--check") {
            checkOnly = true;
        } else if (arg == "--install") {
            installOnly = true;
        } else if (arg == "--install-vdd") {
            installVdd = true;
        }
    }

    std::wstring exeDir = Updater::getExecutableDir();
    std::wstring dllPath = exeDir + L"\\winmirror_rt.dll";
    wchar_t exePathW[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePathW, MAX_PATH);

    if (updateOnly) {
        Updater::ensureCompanionDll(exeDir);
        Updater::checkForUpdates(true);
        return 0;
    }

    std::string netSummary, primaryAddr;
    std::string ipsCsv = collectLocalIpsCsv(port, !checkOnly && !installOnly && !installVdd, &netSummary, &primaryAddr);

    if (checkUpdate || installOnly || installVdd) {
        Updater::ensureCompanionDll(exeDir);
    }
    if (!skipStartupUpdate && checkUpdate && !checkOnly && !installVdd) {
        if (gui) {
            gui->setStatusBanner("Checking for updates...");
        }
        Updater::checkForUpdates(true, gui ? [&](const std::string& msg) { gui->setStatusBanner(msg); }
                                           : Updater::StatusCallback{});
    }

    auto& rt = WinMirrorRuntimeApi::instance();
    bool rtLoaded = rt.load(dllPath);
    if (rtLoaded) {
        rt.enableHighResTimer();
    } else {
        timeBeginPeriod(1);
    }

    WmPreflightReport preflightRep = {};
    if (rtLoaded) {
        rt.runPreflight(exePathW, !checkOnly, preflightRep);
        printPreflightReport(preflightRep, true);
    } else {
        std::cout << "[Preflight] Runtime DLL winmirror_rt.dll not loaded (built-in 1ms timer active)." << std::endl;
    }
    g_hasVddState.store(preflightRep.hasVddDriver != 0);

    if (gui) {
        WinMirrorStatus initSt;
        initSt.isAdmin = isAdmin;
        initSt.gameBoost = gameBoost;
        initSt.ecoQosOff = g_ecoQosDisabled.load();
        initSt.firewallOk = (preflightRep.firewallOk != 0);
        initSt.hasVdd = g_hasVddState.load();
        initSt.dllLoaded = rtLoaded;
        initSt.dllVersion = rtLoaded ? preflightRep.runtimeVersion : "";
        initSt.endpointsSummary = netSummary;
        initSt.primaryConnectAddr = primaryAddr;
        initSt.statusBanner = "Initializing GPU screen capture & 200 Mbps NVENC game engine...";
        gui->updateStatus(initSt);
    }

    auto shutdownTimerAndRuntime = [&]() {
        if (rtLoaded) {
            rt.disableHighResTimer();
            rt.unload();
        } else {
            timeEndPeriod(1);
        }
    };

    if (installVdd) {
        if (!rtLoaded) {
            std::cerr << "[Driver] Cannot install Virtual Display Driver without winmirror_rt.dll." << std::endl;
            shutdownTimerAndRuntime();
            return 1;
        }
        bool vddOk = installVirtualDisplayDriver(exeDir);
        rt.runPreflight(exePathW, true, preflightRep);
        printPreflightReport(preflightRep, true);
        shutdownTimerAndRuntime();
        return vddOk ? 0 : 1;
    }

    if (checkOnly || installOnly) {
        if (installOnly && rtLoaded && !preflightRep.hasVddDriver) {
            if (preflightRep.isAdmin) {
                if (installVirtualDisplayDriver(exeDir)) {
                    rt.runPreflight(exePathW, true, preflightRep);
                    printPreflightReport(preflightRep, true);
                }
            } else {
                std::cout << "[Driver] Run elevated (--install as Administrator) to install 4K HDR Virtual Display Driver." << std::endl;
            }
        }
        shutdownTimerAndRuntime();
        return 0;
    }

    ScreenCapture capture;
    if (!capture.init()) {
        std::cerr << "[Error] Failed to initialize screen capture!" << std::endl;
        shutdownTimerAndRuntime();
        return 1;
    }

    // Acquire initial frame cleanly so exact pixel format (HDR float16 / RGB10 / BGRA8) is settled
    for (int attempt = 0; attempt < 15 && !capture.acquireFrame(16); ++attempt) {}

    bool unlimitedFps = false;
    uint32_t monitorHz = capture.getRefreshRateHz();
    uint32_t targetFps = resolveTargetFps(cliFps, monitorHz, unlimitedFps);

    Encoder encoder;
    encoder.setGameLowLatency(gameBoost);
    if (!encoder.init(capture.getD3DDevice(), capture.getD3DContext(),
                      capture.getWidth(), capture.getHeight(),
                      capture.getPixelFormat(), targetFps,
                      capture.isHdrOutput())) {
        std::cerr << "[Error] Failed to initialize Media Foundation H.264/HEVC Encoder!" << std::endl;
        shutdownTimerAndRuntime();
        return 1;
    }

    RawFrame cpuFallbackFrame;
    std::vector<uint8_t> annexB;
    auto encodeCapturedFrame = [&](uint64_t ptsUs, bool& outIsKey) -> bool {
        ID3D11Device* snapDev = nullptr;
        ID3D11DeviceContext* snapCtx = nullptr;
        ID3D11Texture2D* snapTex = nullptr;
        uint32_t snapW = 0, snapH = 0;
        DXGI_FORMAT snapFmt = DXGI_FORMAT_B8G8R8A8_UNORM;
        bool snapHdr = false;
        bool haveGpu = capture.grabGpuFrame(&snapDev, &snapCtx, &snapTex, snapW, snapH, snapFmt, snapHdr);

        if ((haveGpu && (snapDev != encoder.getCaptureDevice() ||
                         (snapW & ~1u) != encoder.getWidth() ||
                         (snapH & ~1u) != encoder.getHeight() ||
                         snapFmt != encoder.getSrcFormat() ||
                         snapHdr != encoder.isHdrSource())) ||
            (!haveGpu && encoder.getCaptureDevice() != nullptr)) {
            uint32_t initW = haveGpu ? snapW : capture.getWidth();
            uint32_t initH = haveGpu ? snapH : capture.getHeight();
            if (initW > 0 && initH > 0) {
                encoder.init(snapDev, snapCtx, initW, initH, snapFmt,
                             static_cast<uint32_t>(targetFps), snapHdr);
            }
        }

        outIsKey = false;
        bool encoded = false;
        if (haveGpu && encoder.hasVideoProcessor()) {
            encoded = encoder.encodeGpuFrame(snapTex, ptsUs, annexB, outIsKey);
        }
        if (!encoded && (!haveGpu || !encoder.hasVideoProcessor()) && capture.copyCurrentFrameToBgra(cpuFallbackFrame)) {
            if ((cpuFallbackFrame.width & ~1u) != encoder.getWidth() ||
                (cpuFallbackFrame.height & ~1u) != encoder.getHeight()) {
                encoder.init(snapDev, snapCtx, cpuFallbackFrame.width, cpuFallbackFrame.height,
                             DXGI_FORMAT_B8G8R8A8_UNORM, static_cast<uint32_t>(targetFps), false);
            }
            encoded = encoder.encodeBgraCpu(cpuFallbackFrame.bgra.data(),
                                            cpuFallbackFrame.width, cpuFallbackFrame.height,
                                            ptsUs, annexB, outIsKey);
        }

        if (snapTex) snapTex->Release();
        if (snapCtx) snapCtx->Release();
        if (snapDev) snapDev->Release();
        return encoded && !annexB.empty();
    };

    // Probe encoder on an initial frame so faulty zero-copy/VP paths fall back before advertising to Mac
    bool warmKey = false;
    for (int w = 0; w < 20 && !encoder.hasEmittedKeyframe(); ++w) {
        if (!encodeCapturedFrame(static_cast<uint64_t>(w) * 16667ULL, warmKey)) {
            encoder.popPendingFrame(annexB, warmKey);
        }
        if (!encoder.hasEmittedKeyframe()) {
            if (w == 0) capture.acquireFrame(16);
            Sleep(8);
        }
    }
    while (encoder.popPendingFrame(annexB, warmKey)) {}
    encoder.requestKeyframe();

    AirPlayServer server(port);

    AudioCapture audioCapture;
    if (audioCapture.init(
        [&server](const uint8_t* pcm, size_t size, uint32_t sr, uint16_t ch) {
            server.broadcastAudio(pcm, size, sr, ch);
        },
        [&server](const std::string& devs) {
            server.broadcastAudioDevices(devs);
        })) {
        server.broadcastAudioDevices(audioCapture.getDeviceListSerialized());
        server.setAudioSourceHandler([&audioCapture](int index) {
            audioCapture.setSourceDeviceIndex(index);
        });
        audioCapture.start();
    } else {
        std::cout << "[AudioCapture] Note: WASAPI loopback not active." << std::endl;
    }

    server.setCongestionHandler([&encoder](bool congested) {
        encoder.notifyNetworkHealth(congested);
    });

    struct FrameShare {
        std::mutex mutex;
        std::condition_variable cv;
        uint64_t seq = 1; // initial frame already acquired above when ready
        std::atomic<uint64_t> consumedSeq{0};
    } frameShare;

    auto notifyKeyframe = [&encoder, &frameShare]() {
        encoder.requestKeyframe();
        frameShare.cv.notify_one();
    };

    BluetoothBridge btBridge(port, notifyKeyframe);
    bool btReady = btBridge.start(ipsCsv, encoder.getWidth(), encoder.getHeight(),
                                  encoder.isHdrActive(), encoder.getEncoderName());

    server.setStreamInfo(encoder.getWidth(), encoder.getHeight(), static_cast<uint16_t>(targetFps),
                         encoder.getCodec(), encoder.getColorProfile(),
                         audioCapture.getSampleRate(), buildHelloFlags(btReady, capture, encoder, unlimitedFps),
                         notifyKeyframe);

    if (!server.start()) {
        std::cerr << "[Error] Failed to start AirPlay server on port " << port << std::endl;
        btBridge.stop();
        shutdownTimerAndRuntime();
        return 1;
    }

    Beacon beacon(port);
    beacon.start();

    std::cout << "[Pipeline] Capture  : " << capture.getAdapterName()
              << " (" << capture.getPixelFormatName() << ", " << capture.getWidth() << "x" << capture.getHeight()
              << " @ " << monitorHz << " Hz)" << std::endl;
    std::cout << "[Pipeline] Encoder  : " << encoder.getEncoderName()
              << " (" << encoder.getTargetBitrateMbps() << " Mbps @ "
              << (unlimitedFps ? "UNLIMITED FPS" : (std::to_string(targetFps) + " FPS target")) << ")" << std::endl;
    std::cout << "[Pipeline] Audio    : " << (audioCapture.isRunning()
                  ? (audioCapture.getActiveDeviceName() + " [" + std::to_string(audioCapture.getSampleRate()) + " Hz]")
                  : "Off") << std::endl;
    std::cout << "[AirPlay]  Ready    : Start WinMirror on Mac (Ctrl+C to stop)\n" << std::endl;

    std::vector<uint8_t> initCursorBgra;
    CursorPayload initCursorMeta = {};
    if (capture.pollCursor(initCursorMeta, initCursorBgra, true)) {
        server.broadcastCursor(initCursorMeta, initCursorBgra);
    }

    g_unlimitedFpsFlag.store(unlimitedFps);
    std::atomic<uint32_t> curTargetFpsAtomic{targetFps};

    // Decoupled high-priority Capture + Cursor thread so NVENC/PCIe stalls never block DXGI ReleaseFrame or mouse input
    std::thread captureThread([&]() {
        ComMtaScope capCom;
        wm_sec::hideCurrentThread();
        HANDLE hCapMmcss = rt.enterMmcss(L"Games");
        std::vector<uint8_t> curBgra;
        CursorPayload curMeta = {};
        auto lastAcqTime = std::chrono::steady_clock::now();
        uint64_t localSeq = 1;
        while (g_running) {
            if (capture.pollCursor(curMeta, curBgra, false)) {
                server.broadcastCursor(curMeta, curBgra);
            }
            if (server.getActiveClients() == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }
            bool unlimNow = g_unlimitedFpsFlag.load();
            bool boostNow = g_gameBoostFlag.load();
            uint32_t fpsNow = std::max<uint32_t>(15u, curTargetFpsAtomic.load());
            auto minCapGap = std::chrono::microseconds((unlimNow || boostNow) ? 0 : (750000ULL / fpsNow));
            uint32_t dxgiWaitMs = boostNow ? ((fpsNow > 90 || unlimNow) ? 2u : 6u)
                                           : ((fpsNow > 60 || unlimNow) ? 2u : 4u);
            auto capNow = std::chrono::steady_clock::now();
            if (localSeq > frameShare.consumedSeq.load() + 1) {
                g_captureSkips.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (!unlimNow && minCapGap.count() > 0 && (capNow - lastAcqTime) < minCapGap) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (capture.acquireFrame(dxgiWaitMs)) {
                lastAcqTime = std::chrono::steady_clock::now();
                {
                    std::lock_guard<std::mutex> lock(frameShare.mutex);
                    frameShare.seq++;
                    localSeq = frameShare.seq;
                }
                frameShare.cv.notify_one();
            }
        }
        rt.leaveMmcss(hCapMmcss);
    });

    HANDLE hEncMmcss = rt.enterMmcss(L"Games");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    auto startTime = std::chrono::steady_clock::now();
    auto lastReport = startTime;
    auto lastIpRefresh = startTime;
    auto nextEncodeTime = startTime;
    uint32_t encodedFrames = 0;
    uint64_t encodedBytes = 0;
    uint32_t keyframesSent = 0;
    float encodeAvgMs = 0.0f;
    float avgFrameKb = 0.0f;
    uint64_t frameSeq = 0;
    uint64_t lastEncodedSeq = 0;
    uint64_t seenStateSeq = g_stateChangedSeq.load();
    uint32_t lastSentW = encoder.getWidth();
    uint32_t lastSentH = encoder.getHeight();
    VideoCodec lastSentCodec = encoder.getCodec();
    ColorProfile lastSentColor = encoder.getColorProfile();
    uint16_t lastSentFlags = buildHelloFlags(btReady, capture, encoder, unlimitedFps);

    auto minFrameGap = std::chrono::microseconds(1000000 / targetFps);
    auto cvWaitBudget = std::chrono::milliseconds((targetFps > 60 || unlimitedFps) ? 4 : 16);

    auto syncStreamMetadata = [&]() {
        uint16_t flagsNow = buildHelloFlags(btBridge.isRadioAvailable(), capture, encoder, unlimitedFps);
        if (encoder.getWidth() != lastSentW ||
            encoder.getHeight() != lastSentH ||
            encoder.getCodec() != lastSentCodec ||
            encoder.getColorProfile() != lastSentColor ||
            flagsNow != lastSentFlags) {
            lastSentW = encoder.getWidth();
            lastSentH = encoder.getHeight();
            lastSentCodec = encoder.getCodec();
            lastSentColor = encoder.getColorProfile();
            lastSentFlags = flagsNow;
            server.setStreamInfo(lastSentW, lastSentH, static_cast<uint16_t>(targetFps),
                                 lastSentCodec, lastSentColor,
                                 audioCapture.getSampleRate(), flagsNow,
                                 notifyKeyframe);
            btBridge.updateMetadata(ipsCsv, lastSentW, lastSentH,
                                    encoder.isHdrActive(), encoder.getEncoderName());
        }
    };

    std::string tempBanner;
    auto tempBannerUntil = std::chrono::steady_clock::now();
    auto setTempBanner = [&](const std::string& msg, int seconds = 6) {
        tempBanner = msg;
        tempBannerUntil = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        if (gui) gui->setStatusBanner(msg);
    };

    auto pushGuiSnapshot = [&](float curFps, float curMbps) {
        if (!gui) return;
        uint16_t hflags = buildHelloFlags(btBridge.isRadioAvailable(), capture, encoder, unlimitedFps);
        WinMirrorStatus st;
        st.streamingReady = true;
        st.isAdmin = isAdmin;
        st.hasVdd = g_hasVddState.load();
        st.installingVdd = g_installingVdd.load();
        st.unlimitedFps = unlimitedFps;
        st.gameBoost = g_gameBoostFlag.load();
        st.ecoQosOff = g_ecoQosDisabled.load();
        st.firewallOk = (preflightRep.firewallOk != 0);
        st.hdrActive = encoder.isHdrActive();
        st.isHevc = (encoder.getCodec() == CODEC_HEVC);
        st.dllLoaded = rtLoaded;
        st.audioActive = audioCapture.isRunning();
        st.btReady = btBridge.isRadioAvailable();
        st.btConnected = btBridge.isBtClientConnected();
        st.clients = server.getActiveClients();
        st.bitrateScalePct = encoder.getBitrateScalePct();
        st.width = encoder.getWidth();
        st.height = encoder.getHeight();
        st.monitorHz = monitorHz;
        st.targetFps = targetFps;
        st.targetMbps = encoder.getTargetBitrateMbps();
        st.audioRate = audioCapture.getSampleRate();
        st.keyframesSent = keyframesSent;
        st.captureSkips = static_cast<uint32_t>(g_captureSkips.load());
        st.actualFps = curFps;
        st.actualMbps = curMbps;
        st.encodeAvgMs = encodeAvgMs;
        st.avgFrameKb = avgFrameKb;
        st.gpuMode = gpuModeTag(hflags);
        st.encoderName = encoder.getEncoderName();
        st.captureName = capture.getAdapterName();
        st.pixelFormat = capture.getPixelFormatName();
        st.audioName = audioCapture.getActiveDeviceName();
        st.btStatus = btBridge.getStatusText();
        st.endpointsSummary = netSummary;
        st.primaryConnectAddr = primaryAddr;
        st.connectedHost = server.getConnectedHost();
        st.receivers = beacon.getTargetsSnapshot(st.connectedHost);
        st.dllVersion = rtLoaded ? preflightRep.runtimeVersion : "";
        auto nowTs = std::chrono::steady_clock::now();
        if (!tempBanner.empty() && nowTs < tempBannerUntil) {
            st.statusBanner = tempBanner;
        } else {
            st.statusBanner = st.clients > 0
                ? (std::string("Mirroring live (Video + Audio) - click active row to disconnect."))
                : "Click a device above to start mirroring (Video + Audio).";
        }
        gui->updateStatus(st);
    };
    pushGuiSnapshot(0.0f, 0.0f);

    auto emitEncoded = [&](std::vector<uint8_t>& pktData, bool isKey, uint64_t ptsUs, float encMs) {
        size_t pktBytes = pktData.size();
        encodedBytes += pktBytes;
        if (isKey) keyframesSent++;
        float kbNow = static_cast<float>(pktBytes) / 1024.0f;
        avgFrameKb = (avgFrameKb <= 0.01f) ? kbNow : (avgFrameKb * 0.88f + kbNow * 0.12f);
        if (encMs >= 0.0f && encMs < 500.0f) {
            encodeAvgMs = (encodeAvgMs <= 0.01f) ? encMs : (encodeAvgMs * 0.85f + encMs * 0.15f);
        }
        auto pkt = std::make_shared<EncodedFrame>();
        pkt->data = std::move(pktData);
        pkt->width = encoder.getWidth();
        pkt->height = encoder.getHeight();
        pkt->ptsMs = static_cast<uint32_t>(ptsUs / 1000ULL);
        pkt->codec = static_cast<uint8_t>(encoder.getCodec());
        pkt->colorProfile = static_cast<uint8_t>(encoder.getColorProfile());
        pkt->isKeyframe = isKey;
        pkt->seq = ++frameSeq;

        server.broadcastVideo(std::move(pkt));
        encodedFrames++;
        syncStreamMetadata();
    };

    while (g_running) {
        bool wantUnlim = g_unlimitedFpsFlag.load();
        bool wantBoost = g_gameBoostFlag.load();
        if (wantBoost != gameBoost) {
            gameBoost = wantBoost;
            applyGameProcessBoost(gameBoost);
            encoder.setGameLowLatency(gameBoost);
            pushGuiSnapshot(0.0f, 0.0f);
        }
        if (wantUnlim != unlimitedFps) {
            unlimitedFps = wantUnlim;
            bool dummy = false;
            targetFps = resolveTargetFps(unlimitedFps ? -1 : (cliFps < 0 ? 0 : cliFps), monitorHz, dummy);
            curTargetFpsAtomic.store(targetFps);
            minFrameGap = std::chrono::microseconds(1000000 / targetFps);
            cvWaitBudget = std::chrono::milliseconds((targetFps > 60 || unlimitedFps) ? 4 : 16);
            nextEncodeTime = std::chrono::steady_clock::now();
            syncStreamMetadata();
            pushGuiSnapshot(0.0f, 0.0f);
        }

        bool hasClients = (server.getActiveClients() > 0);
        {
            std::optional<AirPlayTarget> targetReq;
            std::string manualIpReq;
            bool rescanNow = false;
            {
                std::lock_guard<std::mutex> aLock(g_actionMtx);
                targetReq = std::move(g_pendingTarget);
                g_pendingTarget.reset();
                manualIpReq = std::move(g_pendingManualIp);
                g_pendingManualIp.clear();
                rescanNow = g_rescanPending;
                g_rescanPending = false;
            }
            if (rescanNow) {
                beacon.triggerRescan();
                setTempBanner("Scanning LAN for AirPlay devices and Mac OBS...", 4);
                pushGuiSnapshot(0.0f, 0.0f);
            }
            if (targetReq.has_value()) {
                const auto& tgt = *targetReq;
                if (tgt.isConnected) {
                    server.disconnectAllClients();
                    hasClients = false;
                    setTempBanner("Disconnected from " + tgt.name + ".");
                } else {
                    if (hasClients) {
                        server.disconnectAllClients();
                        Sleep(25);
                        hasClients = false;
                    }
                    int tryPort = tgt.isDirectObs ? tgt.port : 7002;
                    if (server.connectToReceiver(tgt.ip, tryPort)) {
                        hasClients = true;
                        notifyKeyframe();
                        setTempBanner("Connected to " + tgt.name + " (Video + Audio)!");
                    } else if (!tgt.isDirectObs) {
                        setTempBanner(tgt.isLaptop
                            ? (tgt.name + ": Open OBS with AirPlay source on Mac, or check Mac firewall.")
                            : (tgt.name + " requires Apple FairPlay PIN. Open OBS on Mac to mirror."));
                    } else {
                        setTempBanner("Could not reach " + tgt.name + " - check OBS is open on Mac.");
                    }
                }
                pushGuiSnapshot(0.0f, 0.0f);
            } else if (!manualIpReq.empty()) {
                std::string host = manualIpReq;
                int manualPort = 7002;
                auto colonPos = host.rfind(':');
                if (colonPos != std::string::npos) {
                    int parsedPort = std::atoi(host.substr(colonPos + 1).c_str());
                    if (parsedPort > 0 && parsedPort <= 65535) manualPort = parsedPort;
                    host = host.substr(0, colonPos);
                }
                if (hasClients) {
                    server.disconnectAllClients();
                    hasClients = false;
                }
                beacon.setManualReceiverIp(host, manualPort);
                if (server.connectToReceiver(host, manualPort)) {
                    hasClients = true;
                    notifyKeyframe();
                    setTempBanner("Connected to receiver (Video + Audio)!");
                } else {
                    setTempBanner("Could not connect to receiver - check OBS is running.");
                }
                pushGuiSnapshot(0.0f, 0.0f);
            }
        }
        if (!hasClients) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            nextEncodeTime = std::chrono::steady_clock::now();
        } else {
            if (!unlimitedFps && !gameBoost) {
                auto nowBefore = std::chrono::steady_clock::now();
                if (nowBefore + std::chrono::microseconds(250) < nextEncodeTime) {
                    std::this_thread::sleep_until(nextEncodeTime);
                }
            }

            uint64_t curSeq = 0;
            {
                std::unique_lock<std::mutex> lock(frameShare.mutex);
                frameShare.cv.wait_for(lock, cvWaitBudget, [&]() {
                    return !g_running || frameShare.seq != lastEncodedSeq || encoder.isAwaitingKeyframe();
                });
                curSeq = frameShare.seq;
            }
            if (!g_running) break;

            bool gotNewScreen = (curSeq != lastEncodedSeq);
            bool needKeyPump = encoder.isAwaitingKeyframe() || !encoder.hasEmittedKeyframe();
            if (gotNewScreen || encoder.hasKeyframeRequest() || needKeyPump) {
                lastEncodedSeq = curSeq;
                frameShare.consumedSeq.store(curSeq);

                auto encodeNow = std::chrono::steady_clock::now();
                uint64_t ptsUs = std::chrono::duration_cast<std::chrono::microseconds>(encodeNow - startTime).count();
                bool isKey = false;
                if (encodeCapturedFrame(ptsUs, isKey)) {
                    float encMs = std::chrono::duration<float, std::milli>(
                        std::chrono::steady_clock::now() - encodeNow).count();
                    emitEncoded(annexB, isKey, ptsUs, encMs);
                }
                syncStreamMetadata();

                if (!gotNewScreen && encoder.isAwaitingKeyframe()) {
                    // Pace calm-desktop keyframe recovery cleanly so async MFT gets input without busy spinning
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }

                if (!unlimitedFps && !gameBoost) {
                    // Drift-free fixed cadence without compounding encode latency into frame period
                    nextEncodeTime += minFrameGap;
                    auto afterEncode = std::chrono::steady_clock::now();
                    if (nextEncodeTime + minFrameGap < afterEncode) {
                        nextEncodeTime = afterEncode;
                    }
                }
            }

            // Always drain async MFT output queue even while desktop is idle so first/queued frames never strand
            bool isKeyPending = false;
            while (encoder.popPendingFrame(annexB, isKeyPending)) {
                uint64_t drainPtsUs = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - startTime).count();
                emitEncoded(annexB, isKeyPending, drainPtsUs, -1.0f);
            }
            syncStreamMetadata();
        }

        auto now = std::chrono::steady_clock::now();
        if (now - lastIpRefresh >= std::chrono::seconds(3)) {
            std::string newSummary, newPrimary;
            std::string newIpsCsv = collectLocalIpsCsv(port, false, &newSummary, &newPrimary);
            if (newIpsCsv != ipsCsv || newSummary != netSummary) {
                ipsCsv = newIpsCsv;
                netSummary = newSummary;
                primaryAddr = newPrimary;
                btBridge.updateMetadata(ipsCsv, encoder.getWidth(), encoder.getHeight(),
                                        encoder.isHdrActive(), encoder.getEncoderName());
            }
            lastIpRefresh = now;
        }
        uint64_t stateNow = g_stateChangedSeq.load();
        if (stateNow != seenStateSeq || now - lastReport >= std::chrono::milliseconds(400)) {
            float elapsedSec = std::max(0.05f, std::chrono::duration<float>(now - lastReport).count());
            float currentFps = encodedFrames / elapsedSec;
            float currentMbps = (encodedBytes * 8.0f) / (elapsedSec * 1000000.0f);

            if (!gui) {
                uint16_t hflags = buildHelloFlags(btBridge.isRadioAvailable(), capture, encoder, unlimitedFps);
                std::cout << "\r[Stream] " << std::fixed << std::setprecision(1) << std::setw(4) << currentFps
                          << " FPS | " << std::setw(4) << currentMbps << " Mbps | "
                          << (encoder.getCodec() == CODEC_HEVC ? "HEVC" : "H264")
                          << " [" << gpuModeTag(hflags) << "] "
                          << (encoder.isHdrActive() ? "HDR10" : "SDR")
                          << " | " << encoder.getWidth() << "x" << encoder.getHeight()
                          << " | Aud:" << (audioCapture.isRunning() ? (std::to_string(audioCapture.getSampleRate() / 1000) + "k") : "Off")
                          << " | BT:" << btBridge.getStatusText()
                          << " | Clients:" << server.getActiveClients()
                          << "   " << std::flush;
            } else {
                pushGuiSnapshot(currentFps, currentMbps);
            }

            seenStateSeq = stateNow;
            encodedFrames = 0;
            encodedBytes = 0;
            lastReport = now;
        }
    }

    rt.leaveMmcss(hEncMmcss);
    frameShare.cv.notify_all();
    if (captureThread.joinable()) {
        captureThread.join();
    }
    beacon.stop();
    btBridge.stop();
    audioCapture.stop();
    server.stop();
    capture.cleanup();
    encoder.cleanup();
    shutdownTimerAndRuntime();

    std::cout << "\n[WinMirror AirPlay] Stopped." << std::endl;
    return 0;
}

static std::vector<std::string> parseWinCmdArgs() {
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    std::vector<std::string> args;
    if (!wargv) {
        args.push_back("winmirror.exe");
        return args;
    }
    args.reserve(static_cast<size_t>(wargc));
    for (int i = 0; i < wargc; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(len > 1 ? static_cast<size_t>(len - 1) : 0, '\0');
        if (len > 1) {
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &s[0], len, nullptr, nullptr);
        }
        args.push_back(std::move(s));
    }
    LocalFree(wargv);
    return args;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    wm_sec::enforceAntiReverse();
    wm_sec::hideCurrentThread();

    std::vector<std::string> args = parseWinCmdArgs();
    bool cliMode = false;
    bool noUpdate = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--check" || args[i] == "--install" ||
            args[i] == "--install-vdd" || args[i] == "--update-only") {
            cliMode = true;
        } else if (args[i] == "--no-update") {
            noUpdate = true;
        }
    }

    if (cliMode) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* fpOut = nullptr;
            FILE* fpErr = nullptr;
            freopen_s(&fpOut, "CONOUT$", "w", stdout);
            freopen_s(&fpErr, "CONOUT$", "w", stderr);
        }
        return runWinMirrorEngine(args, nullptr, false);
    }

    if (!noUpdate) {
        WinMirrorGui::runPreLaunchUpdater([&](std::function<void(const std::string&)> setStatus) {
            cleanupPreviousUpdateBinary();
            std::wstring exeDir = Updater::getExecutableDir();
            Updater::checkForUpdates(true, setStatus);
            setStatus("Verifying runtime components...");
            Updater::ensureCompanionDll(exeDir);
        });
    }
    wm_sec::enforceAntiReverse();

    WinMirrorGui gui;
    if (!gui.createAndShow()) {
        return runWinMirrorEngine(args, nullptr, !noUpdate);
    }
    g_trayCleanupGui = &gui;
    std::atexit([]() { if (g_trayCleanupGui) g_trayCleanupGui->removeTrayIcon(); });

    GuiLogStreamBuf logBuf(&gui);
    std::streambuf* oldCout = std::cout.rdbuf(&logBuf);
    std::streambuf* oldCerr = std::cerr.rdbuf(&logBuf);

    std::mutex engineJoinMtx;
    std::mutex updateMtx;
    std::thread updateThread;
    std::atomic<bool> checkingUpdate{false};
    std::thread engineThread;

    auto joinEngineSafe = [&]() {
        std::lock_guard<std::mutex> jLock(engineJoinMtx);
        if (engineThread.joinable()) engineThread.join();
    };

    gui.setActions(
        [&]() {
            g_running = false;
        },
        [&](const AirPlayTarget& target) {
            {
                std::lock_guard<std::mutex> aLock(g_actionMtx);
                g_pendingTarget = target;
                g_pendingManualIp.clear();
            }
            gui.setStatusBanner(target.isConnected
                ? ("Disconnecting from " + target.name + "...")
                : ("Connecting to " + target.name + "..."));
            g_stateChangedSeq++;
        },
        [&](const std::string& ipText) {
            {
                std::lock_guard<std::mutex> aLock(g_actionMtx);
                g_pendingManualIp = ipText;
                g_pendingTarget.reset();
            }
            gui.setStatusBanner("Connecting to receiver...");
            g_stateChangedSeq++;
        },
        [&]() {
            {
                std::lock_guard<std::mutex> aLock(g_actionMtx);
                g_rescanPending = true;
            }
            gui.setStatusBanner("Scanning LAN for AirPlay devices...");
            g_stateChangedSeq++;
        },
        [&]() {
            if (checkingUpdate.exchange(true)) return;
            std::lock_guard<std::mutex> lock(updateMtx);
            if (updateThread.joinable()) updateThread.join();
            gui.setStatusBanner("Checking for updates...");
            updateThread = std::thread([&]() {
                wm_sec::hideCurrentThread();
                wm_sec::enforceAntiReverse();
                bool hasNew = Updater::checkForUpdates(false, [&](const std::string& msg) {
                    gui.setStatusBanner(msg);
                });
                if (hasNew) {
                    gui.setStatusBanner("Update found! Stopping stream & installing...");
                    g_running = false;
                    joinEngineSafe();
                    gui.removeTrayIcon();
                    if (!Updater::checkForUpdates(true, [&](const std::string& msg) {
                        gui.setStatusBanner(msg);
                    })) {
                        gui.setStatusBanner("Update download failed. Please retry.");
                    }
                } else {
                    gui.setStatusBanner(std::string("WinMirror v") + WINMIRROR_VERSION + " is up to date.");
                }
                checkingUpdate.store(false);
            });
        }
    );

    engineThread = std::thread([&]() {
        runWinMirrorEngine(args, &gui, !noUpdate);
    });

    int rc = gui.runMessageLoop();
    g_running = false;
    joinEngineSafe();
    {
        std::lock_guard<std::mutex> lock(updateMtx);
        if (updateThread.joinable()) updateThread.join();
    }
    std::cout.rdbuf(oldCout);
    std::cerr.rdbuf(oldCerr);
    g_trayCleanupGui = nullptr;
    return rc;
}
