#include "Beacon.h"
#include "MdnsParser.h"
#include <iphlpapi.h>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

struct LocalAdapter {
    std::string ipStr;
    uint32_t ipNet = 0;
    uint32_t maskNet = 0;
};

bool isVirtualAdapterDesc(const char* desc) {
    if (!desc || !desc[0]) return false;
    std::string lower(desc);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const char* const kSkip[] = {
        "vethernet", "hyper-v", "vmware", "virtualbox",
        "wintun", "tailscale", "zerotier", "tap-", "wsl"
    };
    for (const char* pat : kSkip) {
        if (lower.find(pat) != std::string::npos) return true;
    }
    return false;
}

std::vector<LocalAdapter> collectLocalAdapters() {
    std::vector<LocalAdapter> out;
    ULONG bufLen = sizeof(IP_ADAPTER_INFO);
    std::vector<BYTE> buf(bufLen);
    PIP_ADAPTER_INFO pAdapter = reinterpret_cast<PIP_ADAPTER_INFO>(buf.data());
    if (GetAdaptersInfo(pAdapter, &bufLen) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
        pAdapter = reinterpret_cast<PIP_ADAPTER_INFO>(buf.data());
    }
    if (GetAdaptersInfo(pAdapter, &bufLen) == NO_ERROR) {
        while (pAdapter) {
            if (isVirtualAdapterDesc(pAdapter->Description)) {
                pAdapter = pAdapter->Next;
                continue;
            }
            const char* ipStr = pAdapter->IpAddressList.IpAddress.String;
            const char* maskStr = pAdapter->IpAddressList.IpMask.String;
            struct in_addr inIp = {}, inMask = {};
            if (inet_pton(AF_INET, ipStr, &inIp) == 1 &&
                inet_pton(AF_INET, maskStr, &inMask) == 1 &&
                inIp.s_addr != 0 &&
                ntohl(inIp.s_addr) != INADDR_LOOPBACK &&
                std::string(ipStr).rfind("169.254.", 0) != 0 &&
                inMask.s_addr != 0) {
                out.push_back({ipStr, inIp.s_addr, inMask.s_addr});
            }
            pAdapter = pAdapter->Next;
        }
    }
    return out;
}

std::vector<uint32_t> buildBroadcastTargets(const std::vector<LocalAdapter>& adapters) {
    std::vector<uint32_t> targets;
    for (const auto& a : adapters) {
        uint32_t bcast = (a.ipNet | ~a.maskNet);
        if (std::find(targets.begin(), targets.end(), bcast) == targets.end()) {
            targets.push_back(bcast);
        }
    }
    uint32_t globalBcast = htonl(INADDR_BROADCAST);
    if (std::find(targets.begin(), targets.end(), globalBcast) == targets.end()) {
        targets.push_back(globalBcast);
    }
    return targets;
}

int scoreReceiverName(const std::string& name) {
    if (name.empty() || name == "MacBook Pro (OBS)") return 1;
    if (detail::mdnsIEquals(name, "OBS")) return 2;
    if (detail::mdnsIContains(name, "macbook") || detail::mdnsIContains(name, "imac")) {
        return (name.find(' ') != std::string::npos && !detail::mdnsIContains(name, "(OBS)")) ? 5 : 4;
    }
    return 3;
}

} // namespace

Beacon::Beacon(int httpPort, int beaconPort)
    : m_httpPort(httpPort), m_beaconPort(beaconPort) {}

Beacon::~Beacon() {
    stop();
}

bool Beacon::initMdnsSocket() {
    m_mdnsSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_mdnsSock == INVALID_SOCKET) return false;

    int reuse = 1;
    setsockopt(m_mdnsSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    u_long nb = 1;
    ioctlsocket(m_mdnsSock, FIONBIO, &nb);

    sockaddr_in bindAddr = {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(5353);
    bindAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_mdnsSock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0) {
        bindAddr.sin_port = 0;
        if (bind(m_mdnsSock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0) {
            closesocket(m_mdnsSock);
            m_mdnsSock = INVALID_SOCKET;
            return false;
        }
        m_mdnsUseUnicast = true;
    } else {
        m_mdnsUseUnicast = false;
        uint32_t mdnsGroup = 0;
        inet_pton(AF_INET, "224.0.0.251", &mdnsGroup);
        auto adapters = collectLocalAdapters();
        if (adapters.empty()) {
            ip_mreq mreq = {};
            mreq.imr_multiaddr.s_addr = mdnsGroup;
            mreq.imr_interface.s_addr = INADDR_ANY;
            setsockopt(m_mdnsSock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));
        } else {
            for (const auto& a : adapters) {
                ip_mreq mreq = {};
                mreq.imr_multiaddr.s_addr = mdnsGroup;
                mreq.imr_interface.s_addr = a.ipNet;
                setsockopt(m_mdnsSock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));
            }
        }
    }
    return true;
}

void Beacon::sendMdnsQuery() {
    if (m_mdnsSock == INVALID_SOCKET) return;
    sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &dst.sin_addr);

    auto qQu = buildMdnsAirPlayPtrQuery(true);
    auto qQm = buildMdnsAirPlayPtrQuery(false);
    auto adapters = collectLocalAdapters();
    if (adapters.empty()) {
        sendto(m_mdnsSock, reinterpret_cast<const char*>(qQu.data()), static_cast<int>(qQu.size()), 0,
               reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        if (!m_mdnsUseUnicast) {
            sendto(m_mdnsSock, reinterpret_cast<const char*>(qQm.data()), static_cast<int>(qQm.size()), 0,
                   reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        }
        return;
    }

    for (const auto& a : adapters) {
        struct in_addr iface = {};
        iface.s_addr = a.ipNet;
        setsockopt(m_mdnsSock, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&iface), sizeof(iface));
        sendto(m_mdnsSock, reinterpret_cast<const char*>(qQu.data()), static_cast<int>(qQu.size()), 0,
               reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        if (!m_mdnsUseUnicast) {
            sendto(m_mdnsSock, reinterpret_cast<const char*>(qQm.data()), static_cast<int>(qQm.size()), 0,
                   reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        }
    }
}

void Beacon::recvMdnsPackets() {
    if (m_mdnsSock == INVALID_SOCKET) return;
    uint8_t buf[4096];
    for (int i = 0; i < 16; ++i) {
        sockaddr_in src = {};
        int srcLen = sizeof(src);
        int r = recvfrom(m_mdnsSock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&src), &srcLen);
        if (r <= 0) break;
        char ipStr[64] = {};
        inet_ntop(AF_INET, &src.sin_addr, ipStr, sizeof(ipStr));
        auto devs = parseMdnsAirPlayResponse(buf, static_cast<size_t>(r), ipStr);
        for (const auto& d : devs) {
            upsertTarget(d.name, d.ip, d.port, d.isLaptop, false, false);
        }
    }
}

void Beacon::upsertTarget(const std::string& name, const std::string& ip, int port,
                          bool isLaptop, bool isDirectObs, bool manual) {
    if (ip.empty()) return;
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(m_rxMutex);

    for (auto& entry : m_targets) {
        if (entry.target.ip == ip) {
            entry.lastSeen = now;
            if (manual) entry.manual = true;
            if (isDirectObs) {
                entry.target.isDirectObs = true;
                entry.target.port = (port > 0 && port <= 65535) ? port : 7002;
            }
            if (isLaptop) entry.target.isLaptop = true;
            if (scoreReceiverName(name) > scoreReceiverName(entry.target.name)) {
                entry.target.name = name;
            }
            return;
        }
    }

    TrackedTarget tt;
    tt.target.name = name.empty() ? ip : name;
    tt.target.ip = ip;
    tt.target.port = (port > 0 && port <= 65535) ? port : 7002;
    tt.target.isLaptop = isLaptop;
    tt.target.isDirectObs = isDirectObs;
    tt.manual = manual;
    tt.lastSeen = now;
    m_targets.push_back(std::move(tt));
}

bool Beacon::start() {
    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sock == INVALID_SOCKET) {
        return false;
    }

    int reuse = 1;
    setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    int broadcast = 1;
    setsockopt(m_sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));
    u_long nb = 1;
    ioctlsocket(m_sock, FIONBIO, &nb);

    sockaddr_in bindAddr = {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(static_cast<u_short>(m_beaconPort));
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bind(m_sock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr));

    initMdnsSocket();

    m_running = true;
    m_thread = std::thread(&Beacon::beaconLoop, this);
    return true;
}

bool Beacon::probeReceiverSubnet(int port) {
    auto adapters = collectLocalAdapters();
    for (const auto& a : adapters) {
        auto dot = a.ipStr.rfind('.');
        if (dot == std::string::npos) continue;
        std::string pfx = a.ipStr.substr(0, dot + 1);
        int selfLast = atoi(a.ipStr.c_str() + dot + 1);

        for (int batchStart = 1; batchStart <= 254 && m_running; batchStart += 64) {
            std::vector<SOCKET> socks;
            std::vector<std::string> ips;
            fd_set wfds;
            FD_ZERO(&wfds);
            for (int host = batchStart; host < batchStart + 64 && host <= 254; ++host) {
                if (host == selfLast) continue;
                std::string ip = pfx + std::to_string(host);
                SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (s == INVALID_SOCKET) continue;
                u_long nb = 1;
                ioctlsocket(s, FIONBIO, &nb);
                sockaddr_in addr = {};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(static_cast<u_short>(port));
                inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
                connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
                FD_SET(s, &wfds);
                socks.push_back(s);
                ips.push_back(std::move(ip));
            }
            std::string foundIp;
            if (!socks.empty()) {
                timeval tv = {0, 120000};
                if (select(0, nullptr, &wfds, nullptr, &tv) > 0) {
                    for (size_t i = 0; i < socks.size(); ++i) {
                        if (FD_ISSET(socks[i], &wfds)) {
                            int err = 0;
                            int errLen = sizeof(err);
                            if (getsockopt(socks[i], SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &errLen) == 0 && err == 0) {
                                foundIp = ips[i];
                                break;
                            }
                        }
                    }
                }
            }
            for (SOCKET s : socks) closesocket(s);
            if (!foundIp.empty()) {
                upsertTarget("MacBook Pro (OBS)", foundIp, port, true, true, false);
                return true;
            }
        }
    }
    return false;
}

bool Beacon::getDiscoveredReceiver(std::string& outName, std::string& outIp, int& outPort) const {
    auto list = getTargetsSnapshot();
    for (const auto& t : list) {
        if (t.isDirectObs) {
            outName = t.name;
            outIp = t.ip;
            outPort = t.port;
            return true;
        }
    }
    return false;
}

std::vector<AirPlayTarget> Beacon::getTargetsSnapshot(const std::string& connectedHost) const {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(m_rxMutex);
    std::vector<AirPlayTarget> res;
    res.reserve(m_targets.size());
    for (const auto& tt : m_targets) {
        bool isConn = (!connectedHost.empty() && tt.target.ip == connectedHost);
        if (!tt.manual && !isConn &&
            std::chrono::duration_cast<std::chrono::seconds>(now - tt.lastSeen).count() > 60) {
            continue;
        }
        AirPlayTarget copy = tt.target;
        copy.isConnected = isConn;
        res.push_back(std::move(copy));
    }
    std::sort(res.begin(), res.end(), [](const AirPlayTarget& a, const AirPlayTarget& b) {
        if (a.isConnected != b.isConnected) return a.isConnected > b.isConnected;
        if (a.isDirectObs != b.isDirectObs) return a.isDirectObs > b.isDirectObs;
        if (a.isLaptop != b.isLaptop) return a.isLaptop > b.isLaptop;
        return a.name < b.name;
    });
    return res;
}

void Beacon::triggerRescan() {
    m_rescanRequested = true;
}

void Beacon::setManualReceiverIp(const std::string& ip, int port) {
    upsertTarget("MacBook Pro (OBS)", ip, port, true, true, true);
}

void Beacon::stop() {
    if (!m_running) return;
    m_running = false;

    if (m_sock != INVALID_SOCKET) {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }
    if (m_mdnsSock != INVALID_SOCKET) {
        closesocket(m_mdnsSock);
        m_mdnsSock = INVALID_SOCKET;
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
    bool rescanNow = false;

    while (m_running) {
        if (tick % 3 == 0 || targets.empty() || rescanNow) {
            targets = buildBroadcastTargets(collectLocalAdapters());
        }
        for (uint32_t targetIp : targets) {
            bcastAddr.sin_addr.s_addr = targetIp;
            sendto(m_sock, message, len, 0, reinterpret_cast<sockaddr*>(&bcastAddr), sizeof(bcastAddr));
        }

        if (tick % 4 == 0 || rescanNow) {
            sendMdnsQuery();
        }

        // Let UDP beacons and mDNS multicast arrive before running a TCP subnet sweep
        for (int slice = 0; slice < 6 && m_running; ++slice) {
            if (m_rescanRequested.exchange(false)) {
                rescanNow = true;
                break;
            }
            fd_set rfds;
            FD_ZERO(&rfds);
            if (m_sock != INVALID_SOCKET) FD_SET(m_sock, &rfds);
            if (m_mdnsSock != INVALID_SOCKET) FD_SET(m_mdnsSock, &rfds);
            timeval tv = {0, 150000};
            if (select(0, &rfds, nullptr, nullptr, &tv) <= 0) continue;

            if (m_sock != INVALID_SOCKET && FD_ISSET(m_sock, &rfds)) {
                for (int rIdx = 0; rIdx < 8; ++rIdx) {
                    char rxBuf[256] = {};
                    sockaddr_in src = {};
                    int srcLen = sizeof(src);
                    int r = recvfrom(m_sock, rxBuf, sizeof(rxBuf) - 1, 0, reinterpret_cast<sockaddr*>(&src), &srcLen);
                    if (r <= 0) break;
                    if (r > 6 && strncmp(rxBuf, "WMOBS:", 6) == 0) {
                        const char* pPort = rxBuf + 6;
                        const char* pColon = strchr(pPort, ':');
                        int rxPort = atoi(pPort);
                        if (rxPort <= 0 || rxPort > 65535) rxPort = 7002;
                        std::string rxName = (pColon && *(pColon + 1)) ? (pColon + 1) : "MacBook Pro (OBS)";
                        char ipStr[64] = {};
                        inet_ntop(AF_INET, &src.sin_addr, ipStr, sizeof(ipStr));
                        upsertTarget(rxName, ipStr, rxPort, true, true, false);
                    }
                }
            }
            if (m_mdnsSock != INVALID_SOCKET && FD_ISSET(m_mdnsSock, &rfds)) {
                recvMdnsPackets();
            }
        }

        if (m_running && (rescanNow || (tick % 6 == 0))) {
            bool hasDirect = false;
            {
                std::lock_guard<std::mutex> lock(m_rxMutex);
                for (const auto& tt : m_targets) {
                    if (tt.target.isDirectObs) {
                        hasDirect = true;
                        break;
                    }
                }
            }
            if (!hasDirect || rescanNow) {
                probeReceiverSubnet(7002);
            }
        }
        rescanNow = false;
        tick++;
    }
}
