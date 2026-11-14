#include "Beacon.h"
#include <iostream>

Beacon::Beacon(int httpPort, int beaconPort)
    : m_httpPort(httpPort), m_beaconPort(beaconPort) {}

Beacon::~Beacon() {
    stop();
}

bool Beacon::start() {
    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sock == INVALID_SOCKET) {
        return false;
    }

    int broadcast = 1;
    setsockopt(m_sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));

    m_running = true;
    m_thread = std::thread(&Beacon::beaconLoop, this);
    return true;
}

void Beacon::stop() {
    if (!m_running) return;
    m_running = false;

    if (m_sock != INVALID_SOCKET) {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void Beacon::beaconLoop() {
    char hostname[128] = "WindowsPC";
    gethostname(hostname, sizeof(hostname));

    sockaddr_in bcastAddr = {};
    bcastAddr.sin_family = AF_INET;
    bcastAddr.sin_port = htons(static_cast<u_short>(m_beaconPort));
    bcastAddr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    char message[256];
    int len = snprintf(message, sizeof(message), "WINMIRROR:%d:%s", m_httpPort, hostname);

    while (m_running) {
        sendto(m_sock, message, len, 0, reinterpret_cast<sockaddr*>(&bcastAddr), sizeof(bcastAddr));
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
