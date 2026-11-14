#pragma once

#include "Common.h"
#include <functional>
#include <thread>
#include <atomic>
#include <string>
#include <vector>

class BluetoothBridge {
public:
    BluetoothBridge(int mediaPort, std::function<void()> onKeyframeReq);
    ~BluetoothBridge();

    bool start(const std::string& ipsCsv, uint32_t width, uint32_t height, bool isHdr, const std::string& codecName);
    void stop();
    void updateMetadata(const std::string& ipsCsv, uint32_t width, uint32_t height, bool isHdr, const std::string& codecName);

    bool isRadioAvailable() const { return m_radioAvailable.load(); }
    bool isBtClientConnected() const { return m_btConnected.load(); }
    std::string getStatusText() const;

private:
    void serverLoop();
    void detectPairedDevices();
    std::string buildHelloLine() const;
    void sendToClient(const std::string& line);

    int m_mediaPort;
    std::function<void()> m_onKeyframeReq;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_radioAvailable{false};
    std::atomic<bool> m_btConnected{false};

    SOCKET m_listenSock = INVALID_SOCKET;
    std::mutex m_clientMutex;
    SOCKET m_clientSock = INVALID_SOCKET;
    std::thread m_thread;

    mutable std::mutex m_metaMutex;
    std::string m_radioName = "Off";
    std::string m_pairedNames;
    std::string m_ipsCsv;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_isHdr = false;
    std::string m_codecName = "H.264";
};
