#pragma once

#include "Common.h"
#include <thread>
#include <atomic>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <functional>

class AirPlayServer {
public:
    AirPlayServer(AudioBuffer& audioBuffer, int port = AIRPLAY_PORT);
    ~AirPlayServer();

    void setStreamInfo(uint32_t width, uint32_t height, uint16_t fps,
                       VideoCodec codec, ColorProfile colorProfile,
                       uint32_t audioSampleRate, bool bluetoothReady,
                       std::function<void()> onKeyframeReq);

    bool start();
    void stop();

    void broadcastVideo(std::shared_ptr<const EncodedFrame> frame);
    void broadcastCursor(const CursorPayload& meta, const std::vector<uint8_t>& bgra);
    void notifyAudioAvailable();

    int getPort() const { return m_port; }
    int getActiveClients() const { return m_activeClients.load(); }

private:
    struct ClientSession {
        SOCKET sock = INVALID_SOCKET;
        std::atomic<bool> alive{true};
        bool waitingForKeyframe = true;

        std::mutex queueMutex;
        std::condition_variable cv;
        std::deque<std::shared_ptr<const EncodedFrame>> videoQueue;

        bool hasCursorPending = false;
        CursorPayload pendingCursor = {};
        std::vector<uint8_t> pendingCursorBgra;
        bool hasAudioPending = false;
        bool hasHelloPending = false;
        bool hasPongPending = false;
        uint32_t pongExtra1 = 0;
        uint32_t pongExtra2 = 0;
    };

    void acceptLoop();
    void runClient(std::shared_ptr<ClientSession> session);
    void clientRecvLoop(std::shared_ptr<ClientSession> session);
    static bool sendAll(SOCKET sock, const void* data, size_t len);
    static bool sendPacket(SOCKET sock, uint16_t type, uint16_t flags, uint32_t extra1, uint32_t extra2, const void* payload, uint32_t payloadSize);

    AudioBuffer& m_audioBuffer;
    int m_port;
    SOCKET m_listenSock = INVALID_SOCKET;
    std::atomic<bool> m_running{false};
    std::atomic<int> m_activeClients{0};
    std::thread m_acceptThread;

    std::mutex m_clientsMutex;
    std::vector<std::shared_ptr<ClientSession>> m_clients;

    // Stream metadata protected by m_infoMutex
    std::mutex m_infoMutex;
    HelloPayload m_hello = {};
    CursorPayload m_lastCursor = {};
    std::vector<uint8_t> m_lastCursorBgra;
    bool m_hasCursor = false;
    std::function<void()> m_onKeyframeReq;
};
