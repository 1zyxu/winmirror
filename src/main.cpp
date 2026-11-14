#include "Common.h"
#include "ScreenCapture.h"
#include "Encoder.h"
#include "HttpServer.h"
#include "Beacon.h"
#include "Updater.h"

#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <iphlpapi.h>
#include <mmsystem.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "winmm.lib")

static std::atomic<bool> g_running{true};

static BOOL WINAPI consoleCtrlHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT) {
        std::cout << "\n[WinMirror] Stopping server..." << std::endl;
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

static void printLocalIps(int port) {
    std::cout << "\n========================================================" << std::endl;
    std::cout << "  WINMIRROR Pro v" << WINMIRROR_VERSION << " -- High Performance 60 FPS" << std::endl;
    std::cout << "========================================================" << std::endl;
    std::cout << "Available Endpoints (USB & LAN/Wi-Fi):" << std::endl;

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
                std::cout << "  -> http://" << ip << ":" << port
                          << " [" << pAdapter->Description << "]" << std::endl;
            }
            pAdapter = pAdapter->Next;
        }
    }
    std::cout << "  -> http://localhost:" << port << " [Loopback / USB Port Forward]" << std::endl;
    std::cout << "========================================================\n" << std::endl;
}

int main(int argc, char* argv[]) {
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
    timeBeginPeriod(1);

    int port = DEFAULT_PORT;
    float quality = 0.55f;
    int targetFps = 60;
    bool checkUpdate = true;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--quality" && i + 1 < argc) {
            quality = std::stof(argv[++i]);
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

    printLocalIps(port);

    ScreenCapture capture;
    if (!capture.init()) {
        std::cerr << "[Error] Failed to initialize screen capture!" << std::endl;
        timeEndPeriod(1);
        return 1;
    }

    Encoder encoder;
    if (!encoder.init(quality)) {
        std::cerr << "[Error] Failed to initialize WIC encoder!" << std::endl;
        timeEndPeriod(1);
        return 1;
    }

    FrameBuffer frameBuffer;

    HttpServer server(frameBuffer, port);
    if (!server.start()) {
        std::cerr << "[Error] Failed to start HTTP streaming server on port " << port << std::endl;
        timeEndPeriod(1);
        return 1;
    }

    Beacon beacon(port);
    beacon.start();

    std::cout << "[WinMirror] Running rock-solid 60 FPS stream (USB & Network active)\n" << std::endl;

    RawFrame rawFrame;
    std::vector<uint8_t> jpegData;

    auto lastReport = std::chrono::steady_clock::now();
    uint32_t capturedFrames = 0;

    const auto frameDuration = std::chrono::microseconds(1000000 / targetFps);

    while (g_running) {
        auto frameStart = std::chrono::steady_clock::now();

        bool hasFrame = capture.capture(rawFrame);
        if (hasFrame) {
            if (encoder.encodeBgraToJpeg(rawFrame.bgra.data(), rawFrame.width, rawFrame.height, jpegData)) {
                frameBuffer.push(std::move(jpegData), rawFrame.width, rawFrame.height);
                capturedFrames++;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        auto now = std::chrono::steady_clock::now();
        if (now - lastReport >= std::chrono::seconds(4)) {
            float elapsedSec = std::chrono::duration<float>(now - lastReport).count();
            float currentFps = capturedFrames / elapsedSec;
            std::cout << "[WinMirror] Live: " << std::fixed << std::setprecision(1) << currentFps << " FPS"
                      << " | Active Viewers: " << server.getActiveClients()
                      << " | GPU: " << (capture.isDxgiActive() ? "DXGI 11" : "GDI") << std::endl;
            capturedFrames = 0;
            lastReport = now;
        }

        auto frameTime = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - frameStart);
        if (frameTime < frameDuration) {
            std::this_thread::sleep_for(frameDuration - frameTime);
        }
    }

    beacon.stop();
    server.stop();
    capture.cleanup();
    encoder.cleanup();
    timeEndPeriod(1);

    std::cout << "[WinMirror] Shutdown complete." << std::endl;
    return 0;
}
