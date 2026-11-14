#include "Common.h"
#include "ScreenCapture.h"
#include "Encoder.h"
#include "AirPlayServer.h"
#include "BluetoothBridge.h"
#include "Beacon.h"
#include "Updater.h"
#include "AudioCapture.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <iphlpapi.h>
#include <mmsystem.h>

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

static uint16_t buildHelloFlags(bool btReady, const ScreenCapture& /*cap*/, const Encoder& enc) {
    uint16_t f = btReady ? HELLO_FLAG_BT : 0;
    if (enc.isNvidiaEncoder()) f |= HELLO_FLAG_NVIDIA;
    if (enc.isZeroCopyActive()) f |= HELLO_FLAG_ZEROCOPY;
    if (enc.isHardwareGpu()) f |= HELLO_FLAG_HW_GPU;
    return f;
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

static std::string collectLocalIpsAndPrint(int port) {
    std::cout << "==================================================================" << std::endl;
    std::cout << "  WINMIRROR AirPlay v" << WINMIRROR_VERSION << " -- GPU HEVC/H264 HDR + Bluetooth" << std::endl;
    std::cout << "==================================================================" << std::endl;
    std::cout << "AirPlay Discovery Transports (USB-C / Router LAN / Wi-Fi / BT):" << std::endl;

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
                std::cout << "  -> airplay://" << ip << ":" << port
                          << " [" << pAdapter->Description << "]" << std::endl;
                if (!firstIp) ipsCsv << ",";
                ipsCsv << ip;
                firstIp = false;
            }
            pAdapter = pAdapter->Next;
        }
    }
    std::cout << "==================================================================\n" << std::endl;
    return ipsCsv.str();
}

int main(int argc, char* argv[]) {
    cleanupPreviousUpdateBinary();
    if (!isRunningAsAdmin()) {
        std::cout << "[WinMirror] Note: Running without Administrator rights (some elevated apps may not mirror)." << std::endl;
    }

    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    timeBeginPeriod(1);

    int port = AIRPLAY_PORT;
    int targetFps = 60;
    bool checkUpdate = true;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--fps" && i + 1 < argc) {
            targetFps = std::stoi(argv[++i]);
        } else if (arg == "--no-update") {
            checkUpdate = false;
        } else if (arg == "--update-only") {
            Updater::checkForUpdates(true);
            return 0;
        }
    }

    if (checkUpdate) {
        std::thread([]() {
            Updater::checkForUpdates(true);
        }).detach();
    }

    std::string ipsCsv = collectLocalIpsAndPrint(port);

    ScreenCapture capture;
    if (!capture.init()) {
        std::cerr << "[Error] Failed to initialize screen capture!" << std::endl;
        timeEndPeriod(1);
        return 1;
    }

    // Acquire initial frame cleanly so exact pixel format (HDR float16 / RGB10 / BGRA8) is settled
    for (int attempt = 0; attempt < 15 && !capture.acquireFrame(16); ++attempt) {}

    Encoder encoder;
    if (!encoder.init(capture.getD3DDevice(), capture.getD3DContext(),
                      capture.getWidth(), capture.getHeight(),
                      capture.getPixelFormat(), static_cast<uint32_t>(targetFps),
                      capture.isHdrOutput())) {
        std::cerr << "[Error] Failed to initialize Media Foundation H.264/HEVC Encoder!" << std::endl;
        timeEndPeriod(1);
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

        if (haveGpu && (snapDev != encoder.getCaptureDevice() ||
                        (snapW & ~1u) != encoder.getWidth() ||
                        (snapH & ~1u) != encoder.getHeight() ||
                        snapFmt != encoder.getSrcFormat() ||
                        snapHdr != encoder.isHdrSource())) {
            encoder.init(snapDev, snapCtx, snapW, snapH, snapFmt,
                         static_cast<uint32_t>(targetFps), snapHdr);
        }

        outIsKey = false;
        bool encoded = false;
        if (haveGpu && encoder.hasVideoProcessor()) {
            encoded = encoder.encodeGpuFrame(snapTex, ptsUs, annexB, outIsKey);
        }
        if (!encoded && !encoder.hasVideoProcessor() && capture.copyCurrentFrameToBgra(cpuFallbackFrame)) {
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
                         audioCapture.getSampleRate(), buildHelloFlags(btReady, capture, encoder),
                         notifyKeyframe);

    if (!server.start()) {
        std::cerr << "[Error] Failed to start AirPlay server on port " << port << std::endl;
        btBridge.stop();
        timeEndPeriod(1);
        return 1;
    }

    Beacon beacon(port);
    beacon.start();

    std::cout << "[Pipeline] Capture  : " << capture.getAdapterName()
              << " (" << capture.getPixelFormatName() << ", " << capture.getWidth() << "x" << capture.getHeight() << ")" << std::endl;
    std::cout << "[Pipeline] Encoder  : " << encoder.getEncoderName()
              << " (" << encoder.getTargetBitrateMbps() << " Mbps @ " << targetFps << " FPS target)" << std::endl;
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
    std::thread captureThread([&]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
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
            if (capture.acquireFrame(4)) {
                {
                    std::lock_guard<std::mutex> lock(frameShare.mutex);
                    frameShare.seq++;
                }
                frameShare.cv.notify_one();
            }
        }
    });

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    auto startTime = std::chrono::steady_clock::now();
    auto lastReport = startTime;
    auto nextEncodeTime = startTime;
    uint32_t encodedFrames = 0;
    uint64_t encodedBytes = 0;
    uint64_t frameSeq = 0;
    uint64_t lastEncodedSeq = 0;
    uint32_t lastSentW = encoder.getWidth();
    uint32_t lastSentH = encoder.getHeight();
    VideoCodec lastSentCodec = encoder.getCodec();
    ColorProfile lastSentColor = encoder.getColorProfile();
    uint16_t lastSentFlags = buildHelloFlags(btReady, capture, encoder);

    const auto minFrameGap = std::chrono::microseconds(1000000 / targetFps);

    auto syncStreamMetadata = [&]() {
        uint16_t flagsNow = buildHelloFlags(btBridge.isRadioAvailable(), capture, encoder);
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
            auto nowBefore = std::chrono::steady_clock::now();
            if (nowBefore + std::chrono::microseconds(400) < nextEncodeTime) {
                std::this_thread::sleep_until(nextEncodeTime);
            }

            uint64_t curSeq = 0;
            {
                std::unique_lock<std::mutex> lock(frameShare.mutex);
                frameShare.cv.wait_for(lock, std::chrono::milliseconds(16), [&]() {
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

                // Drift-free fixed cadence without compounding encode latency into frame period
                nextEncodeTime += minFrameGap;
                auto afterEncode = std::chrono::steady_clock::now();
                if (nextEncodeTime + minFrameGap < afterEncode) {
                    nextEncodeTime = afterEncode;
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
        if (now - lastReport >= std::chrono::milliseconds(500)) {
            float elapsedSec = std::chrono::duration<float>(now - lastReport).count();
            float currentFps = encodedFrames / elapsedSec;
            float currentMbps = (encodedBytes * 8.0f) / (elapsedSec * 1000000.0f);

            uint16_t hflags = buildHelloFlags(btBridge.isRadioAvailable(), capture, encoder);
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
    timeEndPeriod(1);

    std::cout << "\n[WinMirror AirPlay] Stopped." << std::endl;
    return 0;
}
