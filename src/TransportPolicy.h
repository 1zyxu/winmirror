#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>

inline bool isUsbOrLinkLocalIp(const std::string& ip) {
    return ip.rfind("169.254.", 0) == 0 ||
           ip.rfind("172.20.10.", 0) == 0 ||
           ip.rfind("192.168.42.", 0) == 0 ||
           ip.rfind("192.168.137.", 0) == 0;
}

inline bool isPreferredPrivateIp(const std::string& ip) {
    if (ip.empty() || ip == "0.0.0.0" || ip.rfind("127.", 0) == 0) return false;
    if (ip.rfind("192.168.", 0) == 0 || ip.rfind("10.", 0) == 0 || ip.rfind("169.254.", 0) == 0) {
        return true;
    }
    if (ip.rfind("172.", 0) == 0) {
        size_t dot = ip.find('.', 4);
        if (dot != std::string::npos && dot > 4) {
            int secondOctet = std::atoi(ip.substr(4, dot - 4).c_str());
            return secondOctet >= 16 && secondOctet <= 31;
        }
    }
    return false;
}

inline int rankTransportScore(const std::string& ip, double rttMs, bool isUsbInterface) {
    if (rttMs < 0.0) return 10000000;
    int score = static_cast<int>(rttMs * 100.0);
    if (isUsbInterface || isUsbOrLinkLocalIp(ip)) {
        score -= 100000;
    } else if (!isPreferredPrivateIp(ip)) {
        score += 50000;
    }
    return score;
}

inline bool shouldAutoSwitchTransport(const std::string& currentIp, bool currentIsUsb, double currentRttMs,
                                      const std::string& candIp, bool candIsUsb, double candRttMs) {
    if (candIp.empty() || candIp == currentIp || candRttMs < 0.0) return false;
    if (!currentIsUsb && candIsUsb) return true;
    if (currentIsUsb && !candIsUsb) return false;
    return currentRttMs > 12.0 && (candRttMs + 6.0) < currentRttMs;
}

inline bool shouldPresentQueuedFrame(size_t remainingQueue, int64_t msSinceLastPresent) {
    // Decode catch-up P-frames for DPB continuity, but only present to compositor at live edge or >= 33ms floor
    return remainingQueue == 0 || msSinceLastPresent >= 33;
}
