#pragma once

#include "Common.h"
#include <thread>
#include <atomic>

class Beacon {
public:
    Beacon(int httpPort = DEFAULT_PORT, int beaconPort = BEACON_PORT);
    ~Beacon();

    bool start();
    void stop();

private:
    void beaconLoop();

    int m_httpPort;
    int m_beaconPort;
    SOCKET m_sock = INVALID_SOCKET;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
};
