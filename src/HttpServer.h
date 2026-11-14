#pragma once

#include "Common.h"
#include <thread>
#include <vector>
#include <atomic>

class HttpServer {
public:
    HttpServer(FrameBuffer& frameBuffer, AudioBuffer& audioBuffer, int port = DEFAULT_PORT);
    ~HttpServer();

    bool start();
    void stop();

    int getPort() const { return m_port; }
    int getActiveClients() const { return m_activeClients.load(); }

private:
    void acceptLoop();
    void handleClient(SOCKET clientSock);
    void handleRaw(SOCKET clientSock);
    void handleStream(SOCKET clientSock);
    void handleIndex(SOCKET clientSock);
    void handleSnapshot(SOCKET clientSock);
    void handleStatus(SOCKET clientSock);
    void handleTriggerUpdate(SOCKET clientSock);
    void handleUpload(SOCKET clientSock, const std::string& initialData);

    FrameBuffer& m_frameBuffer;
    AudioBuffer& m_audioBuffer;
    int m_port;
    SOCKET m_listenSock = INVALID_SOCKET;
    std::atomic<bool> m_running{false};
    std::atomic<int> m_activeClients{0};
    std::thread m_acceptThread;
};
