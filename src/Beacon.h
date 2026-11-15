#pragma once

#include "Common.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Beacon {
public:
    Beacon(int airplayPort = AIRPLAY_PORT, int beaconPort = BEACON_PORT);
    ~Beacon();

    bool start();
    void stop();
    bool getDiscoveredReceiver(std::string& outName, std::string& outIp, int& outPort) const;
    std::vector<AirPlayTarget> getTargetsSnapshot(const std::string& connectedHost = "") const;
    void triggerRescan();
    void setManualReceiverIp(const std::string& ip, int port = 7002);
    bool probeReceiverSubnet(int port = 7002);

private:
    struct TrackedTarget {
        AirPlayTarget target;
        bool manual = false;
        std::chrono::steady_clock::time_point lastSeen;
    };

    void beaconLoop();
    bool initMdnsSocket();
    void sendMdnsQuery();
    void recvMdnsPackets();
    void upsertTarget(const std::string& name, const std::string& ip, int port,
                      bool isLaptop, bool isDirectObs, bool manual = false);

    int m_httpPort;
    int m_beaconPort;
    SOCKET m_sock = INVALID_SOCKET;
    SOCKET m_mdnsSock = INVALID_SOCKET;
    bool m_mdnsUseUnicast = false;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_rescanRequested{false};
    std::thread m_thread;
    mutable std::mutex m_rxMutex;
    std::vector<TrackedTarget> m_targets;
};
