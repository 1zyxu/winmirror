#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

struct ParsedMdnsAirPlayDevice {
    std::string name;
    std::string host;
    std::string ip;
    std::string model;
    int port = 7000;
    bool isLaptop = false;
};

namespace detail {

inline bool mdnsIEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

inline bool mdnsIContains(const std::string& hay, const std::string& needle) {
    if (needle.empty() || hay.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(hay[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

inline uint16_t readU16BE(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline bool readDnsName(const uint8_t* pkt, size_t len, size_t offset,
                        std::string& outName, size_t& outNextOffset) {
    outName.clear();
    size_t pos = offset;
    size_t nextAfterFirstPtr = 0;
    bool jumped = false;
    int jumps = 0;

    while (pos < len) {
        uint8_t b = pkt[pos];
        if (b == 0) {
            if (!jumped) nextAfterFirstPtr = pos + 1;
            outNextOffset = nextAfterFirstPtr;
            return true;
        }
        if ((b & 0xC0) == 0xC0) {
            if (pos + 1 >= len) return false;
            uint16_t ptr = static_cast<uint16_t>(((b & 0x3F) << 8) | pkt[pos + 1]);
            if (ptr >= len) return false;
            if (!jumped) {
                nextAfterFirstPtr = pos + 2;
                jumped = true;
            }
            pos = ptr;
            if (++jumps >= 16) return false;
            continue;
        }
        if ((b & 0xC0) != 0) return false;
        if (pos + 1 + b > len) return false;
        if (!outName.empty()) outName.push_back('.');
        outName.append(reinterpret_cast<const char*>(pkt + pos + 1), b);
        pos += 1 + b;
    }
    return false;
}

inline std::string stripAirPlaySuffix(const std::string& fullName) {
    static const char kSuffix[] = "._airplay._tcp.local";
    const size_t suffixLen = sizeof(kSuffix) - 1;
    if (fullName.size() <= suffixLen) return {};
    if (!mdnsIEquals(fullName.substr(fullName.size() - suffixLen), kSuffix)) return {};
    return fullName.substr(0, fullName.size() - suffixLen);
}

inline bool looksLikeMacModelOrName(const std::string& model, const std::string& name) {
    if (mdnsIContains(model, "macbook") || mdnsIContains(model, "imac") ||
        mdnsIContains(model, "macmini") || mdnsIContains(model, "macpro") ||
        mdnsIContains(model, "macstudio") || mdnsIContains(model, "mac")) {
        return !mdnsIContains(model, "appletv");
    }
    return mdnsIContains(name, "macbook") || mdnsIContains(name, "imac") ||
           mdnsIContains(name, "mac mini") || mdnsIContains(name, "mac studio") ||
           mdnsIContains(name, "mac pro");
}

} // namespace detail

inline std::vector<ParsedMdnsAirPlayDevice> parseMdnsAirPlayResponse(
    const uint8_t* pkt, size_t len, const std::string& fallbackIp) {
    std::vector<ParsedMdnsAirPlayDevice> out;
    if (!pkt || len < 12) return out;

    uint16_t flags = detail::readU16BE(pkt + 2);
    if ((flags & 0x8000) == 0) return out; // only DNS responses

    uint16_t qdCount = detail::readU16BE(pkt + 4);
    uint16_t anCount = detail::readU16BE(pkt + 6);
    uint16_t nsCount = detail::readU16BE(pkt + 8);
    uint16_t arCount = detail::readU16BE(pkt + 10);

    size_t pos = 12;
    for (uint16_t i = 0; i < qdCount; ++i) {
        std::string qname;
        size_t next = 0;
        if (!detail::readDnsName(pkt, len, pos, qname, next) || next + 4 > len) return out;
        pos = next + 4;
    }

    struct InstanceInfo {
        std::string friendlyName;
        std::string targetHost;
        std::string model;
        int port = 7000;
        bool hasAirPlay = false;
    };
    std::map<std::string, InstanceInfo> instances;
    std::map<std::string, std::string> hostToIpv4;

    uint32_t totalRRs = static_cast<uint32_t>(anCount) + nsCount + arCount;
    for (uint32_t i = 0; i < totalRRs && pos < len; ++i) {
        std::string rrName;
        size_t next = 0;
        if (!detail::readDnsName(pkt, len, pos, rrName, next) || next + 10 > len) break;

        uint16_t rtype = detail::readU16BE(pkt + next);
        uint16_t rdlen = detail::readU16BE(pkt + next + 8);
        size_t rdStart = next + 10;
        if (rdStart + rdlen > len) break;
        pos = rdStart + rdlen;

        std::string lowerName = rrName;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (rtype == 12 && detail::mdnsIEquals(rrName, "_airplay._tcp.local")) { // PTR
            std::string instFull;
            size_t dummy = 0;
            if (detail::readDnsName(pkt, len, rdStart, instFull, dummy)) {
                std::string friendly = detail::stripAirPlaySuffix(instFull);
                if (!friendly.empty()) {
                    std::string key = instFull;
                    std::transform(key.begin(), key.end(), key.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    auto& info = instances[key];
                    info.friendlyName = friendly;
                    info.hasAirPlay = true;
                }
            }
        } else if (rtype == 33 && rdlen >= 6) { // SRV
            std::string friendly = detail::stripAirPlaySuffix(rrName);
            if (!friendly.empty()) {
                uint16_t port = detail::readU16BE(pkt + rdStart + 4);
                std::string target;
                size_t dummy = 0;
                if (detail::readDnsName(pkt, len, rdStart + 6, target, dummy)) {
                    auto& info = instances[lowerName];
                    if (info.friendlyName.empty()) info.friendlyName = friendly;
                    info.targetHost = target;
                    info.port = port ? static_cast<int>(port) : 7000;
                    info.hasAirPlay = true;
                }
            }
        } else if (rtype == 16) { // TXT
            std::string friendly = detail::stripAirPlaySuffix(rrName);
            if (!friendly.empty()) {
                auto& info = instances[lowerName];
                if (info.friendlyName.empty()) info.friendlyName = friendly;
                info.hasAirPlay = true;
                size_t t = rdStart;
                while (t < rdStart + rdlen) {
                    uint8_t slen = pkt[t++];
                    if (t + slen > rdStart + rdlen) break;
                    std::string kv(reinterpret_cast<const char*>(pkt + t), slen);
                    t += slen;
                    if (kv.size() > 6 && detail::mdnsIEquals(kv.substr(0, 6), "model=")) {
                        info.model = kv.substr(6);
                    }
                }
            }
        } else if (rtype == 1 && rdlen == 4) { // A
            char ipBuf[32];
            snprintf(ipBuf, sizeof(ipBuf), "%u.%u.%u.%u",
                     pkt[rdStart], pkt[rdStart + 1], pkt[rdStart + 2], pkt[rdStart + 3]);
            hostToIpv4[lowerName] = ipBuf;
        }
    }

    for (const auto& kv : instances) {
        const auto& info = kv.second;
        if (!info.hasAirPlay || info.friendlyName.empty()) continue;
        ParsedMdnsAirPlayDevice dev;
        dev.name = info.friendlyName;
        dev.host = info.targetHost;
        dev.port = info.port;
        dev.model = info.model;
        dev.isLaptop = detail::looksLikeMacModelOrName(info.model, info.friendlyName);

        std::string hostLower = info.targetHost;
        std::transform(hostLower.begin(), hostLower.end(), hostLower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto it = hostToIpv4.find(hostLower);
        if (it != hostToIpv4.end()) {
            dev.ip = it->second;
        } else {
            dev.ip = fallbackIp;
        }
        if (!dev.ip.empty()) {
            out.push_back(std::move(dev));
        }
    }
    return out;
}

inline std::vector<uint8_t> buildMdnsAirPlayPtrQuery(bool unicastResponse) {
    const uint8_t qclassLow = 0x01;
    const uint8_t qclassHigh = unicastResponse ? 0x80 : 0x00;
    return {
        0x00, 0x00, // ID
        0x00, 0x00, // Standard query
        0x00, 0x01, // QDCOUNT = 1
        0x00, 0x00, // ANCOUNT = 0
        0x00, 0x00, // NSCOUNT = 0
        0x00, 0x00, // ARCOUNT = 0
        0x08, '_', 'a', 'i', 'r', 'p', 'l', 'a', 'y',
        0x04, '_', 't', 'c', 'p',
        0x05, 'l', 'o', 'c', 'a', 'l',
        0x00,
        0x00, 0x0C, // PTR
        qclassHigh, qclassLow
    };
}
