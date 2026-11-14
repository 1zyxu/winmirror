#include "Beacon.h"
#include <iphlpapi.h>
#include <vector>
#include <algorithm>
#include <iostream>

static std::vector<uint32_t> collectBroadcastTargets() {
    std::vector<uint32_t> targets;
    ULONG bufLen = sizeof(IP_ADAPTER_INFO);
    std::vector<BYTE> buf(bufLen);
    PIP_ADAPTER_INFO pAdapter = reinterpret_cast<PIP_ADAPTER_INFO>(buf.data());
    if (GetAdaptersInfo(pAdapter, &bufLen) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
        pAdapter = reinterpret_cast<PIP_ADAPTER_INFO>(buf.data());
    }
    if (GetAdaptersInfo(pAdapter, &bufLen) == NO_ERROR) {
        while (pAdapter) {
            const char* ipStr = pAdapter->IpAddressList.IpAddress.String;
            const char* maskStr = pAdapter->IpAddressList.IpMask.String;
            struct in_addr inIp = {}, inMask = {};
            if (inet_pton(AF_INET, ipStr, &inIp) == 1 &&
                inet_pton(AF_INET, maskStr, &inMask) == 1 &&
                inIp.s_addr != 0 &&
                ntohl(inIp.s_addr) != INADDR_LOOPBACK &&
                inMask.s_addr != 0) {
                uint32_t bcast = (inIp.s_addr | ~inMask.s_addr);
                if (std::find(targets.begin(), targets.end(), bcast) == targets.end()) {
                    targets.push_back(bcast);
                }
            }
            pAdapter = pAdapter->Next;
        }
    }
    uint32_t globalBcast = htonl(INADDR_BROADCAST);
    if (std::find(targets.begin(), targets.end(), globalBcast) == targets.end()) {
        targets.push_back(globalBcast);
    }
    return targets;
}

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
    int len = snprintf(message, sizeof(message), "WINMIRROR2:%d:%s", m_httpPort, hostname);

    std::vector<uint32_t> targets;
    int tick = 0;
    while (m_running) {
        if (tick % 3 == 0 || targets.empty()) {
            targets = collectBroadcastTargets();
        }
        for (uint32_t targetIp : targets) {
            bcastAddr.sin_addr.s_addr = targetIp;
            sendto(m_sock, message, len, 0, reinterpret_cast<sockaddr*>(&bcastAddr), sizeof(bcastAddr));
        }
        tick++;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
