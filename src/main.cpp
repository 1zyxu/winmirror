#include "Common.h"
#include "CapturePolicy.h"
#include "ScreenCapture.h"
#include "Encoder.h"
#include "AirPlayServer.h"
#include "BluetoothBridge.h"
#include "Beacon.h"
#include "Updater.h"
#include "AudioCapture.h"
#include "WinMirrorRuntime.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <iphlpapi.h>
#include <mmsystem.h>

// Signal NVIDIA Optimus and AMD Switchable Graphics drivers to expose the discrete GPU
extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

static std::atomic<bool> g_running{true};

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

static void cleanupPreviousUpdateBinary() {
    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH) > 0) {
        std::string oldExe = std::string(exePath) + ".old.exe";
        DeleteFileA(oldExe.c_str());
    }
}

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

static std::string collectLocalIpsCsv(int port, bool printBanner) {
    if (printBanner) {
        std::cout << "==================================================================" << std::endl;
        std::cout << "  WINMIRROR AirPlay v" << WINMIRROR_VERSION << " -- GPU HEVC/H264 HDR + Bluetooth" << std::endl;
        std::cout << "==================================================================" << std::endl;
        std::cout << "AirPlay Discovery Transports (USB-C / Router LAN / Wi-Fi / BT):" << std::endl;
    }

    std::ostringstream ipsCsv;
    ULONG bufLen = sizeof(IP_ADAPTER_INFO);
    std::vector<BYTE> buf(bufLen);
    PIP_ADAPTER_INFO pAdapter = reinterpret_cast<PIP_ADAPTER_INFO>(buf.data());

    if (GetAdaptersInfo(pAdapter, &bufLen) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
        pAdapter = reinterpret_cast<PIP_ADAPTER_INFO>(buf.data());
    }

    bool firstIp = true;
    if (GetAdaptersInfo(pAdapter, &bufLen) == NO_ERROR) {
        while (pAdapter) {
            std::string ip = pAdapter->IpAddressList.IpAddress.String;
            if (ip != "0.0.0.0" && ip != "127.0.0.1") {
                if (printBanner) {
                    std::cout << "  -> airplay://" << ip << ":" << port
                              << " [" << pAdapter->Description << "]" << std::endl;
                }
                if (!firstIp) ipsCsv << ",";
                ipsCsv << ip;
                firstIp = false;
            }
            pAdapter = pAdapter->Next;
        }
    }
    if (printBanner) {
        std::cout << "==================================================================\n" << std::endl;
    }
    return ipsCsv.str();
}

int main(int argc, char* argv[]) {
    cleanupPreviousUpdateBinary();
    if (!isRunningAsAdmin()) {
        std::cout << "[WinMirror] Note: Running without Administrator rights (some elevated apps may not mirror)." << std::endl;
    }

    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    int port = AIRPLAY_PORT;
    int cliFps = 0; // 0 = auto native monitor refresh rate, -1 = unlimited event-driven FPS
    bool checkUpdate = true;
    bool checkOnly = false;
    bool installOnly = false;
    bool installVdd = false;
    bool updateOnly = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--fps" && i + 1 < argc) {
            cliFps = parseCliFps(argv[++i]);
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

    std::string ipsCsv = collectLocalIpsCsv(port, !checkOnly && !installOnly && !installVdd);

    if (checkUpdate || installOnly || installVdd) {
        Updater::ensureCompanionDll(exeDir);
    }
    if (checkUpdate && !checkOnly && !installVdd) {
        Updater::checkForUpdates(true);
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

    struct FrameShare {
        std::mutex mutex;
        std::condition_variable cv;
        uint64_t seq = 1; // initial frame already acquired above when ready
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

    // Decoupled high-priority Capture + Cursor thread so NVENC/PCIe stalls never block DXGI ReleaseFrame or mouse input
    uint32_t dxgiWaitMs = (targetFps > 60 || unlimitedFps) ? 1u : 4u;
    std::thread captureThread([&]() {
        HANDLE hCapMmcss = rt.enterMmcss(L"Games");
        std::vector<uint8_t> curBgra;
        CursorPayload curMeta = {};
        while (g_running) {
            if (capture.pollCursor(curMeta, curBgra, false)) {
                server.broadcastCursor(curMeta, curBgra);
            }
            if (server.getActiveClients() == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }
            if (capture.acquireFrame(dxgiWaitMs)) {
                {
                    std::lock_guard<std::mutex> lock(frameShare.mutex);
                    frameShare.seq++;
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
    uint64_t frameSeq = 0;
    uint64_t lastEncodedSeq = 0;
    uint32_t lastSentW = encoder.getWidth();
    uint32_t lastSentH = encoder.getHeight();
    VideoCodec lastSentCodec = encoder.getCodec();
    ColorProfile lastSentColor = encoder.getColorProfile();
    uint16_t lastSentFlags = buildHelloFlags(btReady, capture, encoder, unlimitedFps);

    const auto minFrameGap = std::chrono::microseconds(1000000 / targetFps);
    const auto cvWaitBudget = std::chrono::milliseconds((targetFps > 60 || unlimitedFps) ? 4 : 16);

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

    auto emitEncoded = [&](std::vector<uint8_t>& pktData, bool isKey, uint64_t ptsUs) {
        encodedBytes += pktData.size();
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
        bool hasClients = (server.getActiveClients() > 0);
        if (!hasClients) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            nextEncodeTime = std::chrono::steady_clock::now();
        } else {
            if (!unlimitedFps) {
                auto nowBefore = std::chrono::steady_clock::now();
                if (nowBefore + std::chrono::microseconds(250) < nextEncodeTime) {
                    std::this_thread::sleep_until(nextEncodeTime);
                }
            }

            uint64_t curSeq = 0;
            {
                std::unique_lock<std::mutex> lock(frameShare.mutex);
                frameShare.cv.wait_for(lock, cvWaitBudget, [&]() {
                    return !g_running || frameShare.seq != lastEncodedSeq || encoder.hasKeyframeRequest();
                });
                curSeq = frameShare.seq;
            }
            if (!g_running) break;

            bool gotNewScreen = (curSeq != lastEncodedSeq);
            if (gotNewScreen || encoder.hasKeyframeRequest() || !encoder.hasEmittedKeyframe()) {
                lastEncodedSeq = curSeq;

                auto encodeNow = std::chrono::steady_clock::now();
                uint64_t ptsUs = std::chrono::duration_cast<std::chrono::microseconds>(encodeNow - startTime).count();
                bool isKey = false;
                if (encodeCapturedFrame(ptsUs, isKey)) {
                    emitEncoded(annexB, isKey, ptsUs);
                }
                syncStreamMetadata();

                if (!unlimitedFps) {
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
                emitEncoded(annexB, isKeyPending, drainPtsUs);
            }
            syncStreamMetadata();
        }

        // Single-line live console status
        auto now = std::chrono::steady_clock::now();
        if (now - lastIpRefresh >= std::chrono::seconds(3)) {
            std::string newIpsCsv = collectLocalIpsCsv(port, false);
            if (newIpsCsv != ipsCsv) {
                ipsCsv = newIpsCsv;
                btBridge.updateMetadata(ipsCsv, encoder.getWidth(), encoder.getHeight(),
                                        encoder.isHdrActive(), encoder.getEncoderName());
            }
            lastIpRefresh = now;
        }
        if (now - lastReport >= std::chrono::milliseconds(500)) {
            float elapsedSec = std::chrono::duration<float>(now - lastReport).count();
            float currentFps = encodedFrames / elapsedSec;
            float currentMbps = (encodedBytes * 8.0f) / (elapsedSec * 1000000.0f);

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
