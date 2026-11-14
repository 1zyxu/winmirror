#pragma once

#include "Common.h"
#include <thread>
#include <vector>
#include <atomic>

class HttpServer {
public:
    HttpServer(FrameBuffer& frameBuffer, int port = DEFAULT_PORT);
    ~HttpServer();

    bool start();
    void stop();

    int getPort() const { return m_port; }
    int getActiveClients() const { return m_activeClients.load(); }

private:
    void acceptLoop();
    void handleClient(SOCKET clientSock);
    void handleStream(SOCKET clientSock);
    void handleIndex(SOCKET clientSock);
    void handleSnapshot(SOCKET clientSock);
    void handleStatus(SOCKET clientSock);

    FrameBuffer& m_frameBuffer;
    int m_port;
    SOCKET m_listenSock = INVALID_SOCKET;
    std::atomic<bool> m_running{false};
    std::atomic<int> m_activeClients{0};
    std::thread m_acceptThread;
};
