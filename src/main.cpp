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
    for (int attempt = 0; attempt < 20 && !capture.acquireFrame(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    Encoder encoder;
    if (!encoder.init(capture.getD3DDevice(), capture.getD3DContext(),
                      capture.getWidth(), capture.getHeight(),
                      capture.getPixelFormat(), static_cast<uint32_t>(targetFps),
                      capture.isHdrOutput())) {
        std::cerr << "[Error] Failed to initialize Media Foundation H.264/HEVC Encoder!" << std::endl;
        timeEndPeriod(1);
        return 1;
    }

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

    BluetoothBridge btBridge(port, [&encoder]() {
        encoder.requestKeyframe();
    });
    bool btReady = btBridge.start(ipsCsv, encoder.getWidth(), encoder.getHeight(),
                                  encoder.isHdrActive(), encoder.getEncoderName());

    server.setStreamInfo(encoder.getWidth(), encoder.getHeight(), static_cast<uint16_t>(targetFps),
                         encoder.getCodec(), encoder.getColorProfile(),
                         audioCapture.getSampleRate(), buildHelloFlags(btReady, capture, encoder),
                         [&encoder]() { encoder.requestKeyframe(); });

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

    auto startTime = std::chrono::steady_clock::now();
    auto lastReport = startTime;
    auto nextEncodeTime = startTime;
    uint32_t encodedFrames = 0;
    uint64_t encodedBytes = 0;
    uint64_t frameSeq = 0;
    bool lastZeroCopy = encoder.isZeroCopyActive();

    const auto minFrameGap = std::chrono::microseconds(1000000 / targetFps);
    const auto idleTick = std::chrono::milliseconds(8);
    const auto activeTick = std::chrono::milliseconds(1);

    RawFrame cpuFallbackFrame;
    std::vector<uint8_t> annexB;
    std::vector<uint8_t> cursorBgra;
    CursorPayload cursorMeta = {};

    if (capture.pollCursor(cursorMeta, cursorBgra, true)) {
        server.broadcastCursor(cursorMeta, cursorBgra);
    }

    while (g_running) {
        auto loopStart = std::chrono::steady_clock::now();

        // 1. Hardware mouse cursor tracking
        if (capture.pollCursor(cursorMeta, cursorBgra, false)) {
            server.broadcastCursor(cursorMeta, cursorBgra);
        }

        // 2. Capture & GPU Hardware Video Encode (at up to targetFps, drift-free)
        if (loopStart + std::chrono::microseconds(800) >= nextEncodeTime) {
            bool gotNewScreen = capture.acquireFrame();
            if (gotNewScreen) {
                // Re-initialize encoder seamlessly if resolution or HDR mode switched (e.g. launching fullscreen game)
                if ((capture.getWidth() & ~1u) != encoder.getWidth() ||
                    (capture.getHeight() & ~1u) != encoder.getHeight() ||
                    capture.getPixelFormat() != encoder.getSrcFormat()) {
                    encoder.init(capture.getD3DDevice(), capture.getD3DContext(),
                                 capture.getWidth(), capture.getHeight(),
                                 capture.getPixelFormat(), static_cast<uint32_t>(targetFps),
                                 capture.isHdrOutput());
                    server.setStreamInfo(encoder.getWidth(), encoder.getHeight(), static_cast<uint16_t>(targetFps),
                                         encoder.getCodec(), encoder.getColorProfile(),
                                         audioCapture.getSampleRate(),
                                         buildHelloFlags(btBridge.isRadioAvailable(), capture, encoder),
                                         [&encoder]() { encoder.requestKeyframe(); });
                    lastZeroCopy = encoder.isZeroCopyActive();
                    btBridge.updateMetadata(ipsCsv, encoder.getWidth(), encoder.getHeight(),
                                            encoder.isHdrActive(), encoder.getEncoderName());
                }
            }

            bool hasClients = (server.getActiveClients() > 0);
            bool encodedThisSlot = false;
            bool gpuPathUsed = false;

            if (hasClients && (gotNewScreen || encoder.hasKeyframeRequest())) {
                uint64_t ptsUs = std::chrono::duration_cast<std::chrono::microseconds>(loopStart - startTime).count();
                bool isKey = false;
                bool encoded = false;

                if (capture.isDxgiActive() && capture.getGpuTexture() && encoder.hasVideoProcessor()) {
                    gpuPathUsed = true;
                    encoded = encoder.encodeGpuFrame(capture.getGpuTexture(), ptsUs, annexB, isKey);
                } else if (capture.copyCurrentFrameToBgra(cpuFallbackFrame)) {
                    encoded = encoder.encodeBgraCpu(cpuFallbackFrame.bgra.data(),
                                                    cpuFallbackFrame.width, cpuFallbackFrame.height,
                                                    ptsUs, annexB, isKey);
                }

                if (encoded && !annexB.empty()) {
                    encodedBytes += annexB.size();
                    auto pkt = std::make_shared<EncodedFrame>();
                    pkt->data = std::move(annexB);
                    pkt->width = encoder.getWidth();
                    pkt->height = encoder.getHeight();
                    pkt->ptsMs = static_cast<uint32_t>(ptsUs / 1000ULL);
                    pkt->codec = static_cast<uint8_t>(encoder.getCodec());
                    pkt->colorProfile = static_cast<uint8_t>(encoder.getColorProfile());
                    pkt->isKeyframe = isKey;
                    pkt->seq = ++frameSeq;

                    server.broadcastVideo(std::move(pkt));
                    encodedFrames++;
                    encodedThisSlot = true;
                    if (encoder.isZeroCopyActive() != lastZeroCopy) {
                        lastZeroCopy = encoder.isZeroCopyActive();
                        server.setStreamInfo(encoder.getWidth(), encoder.getHeight(), static_cast<uint16_t>(targetFps),
                                             encoder.getCodec(), encoder.getColorProfile(),
                                             audioCapture.getSampleRate(),
                                             buildHelloFlags(btBridge.isRadioAvailable(), capture, encoder),
                                             [&encoder]() { encoder.requestKeyframe(); });
                    }
                }
            }

            // Advance slot when idle, frame produced, or GPU pipeline consumed the current screen frame
            if (!hasClients || encodedThisSlot || (gpuPathUsed && gotNewScreen)) {
                nextEncodeTime += minFrameGap;
                if (nextEncodeTime + minFrameGap < loopStart) {
                    nextEncodeTime = loopStart + minFrameGap;
                }
            }
        }

        // 3. Single-line live console status
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

        auto targetSleep = (server.getActiveClients() > 0) ? activeTick : idleTick;
        auto spent = std::chrono::steady_clock::now() - loopStart;
        if (spent < targetSleep) {
            std::this_thread::sleep_for(targetSleep - spent);
        }
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
