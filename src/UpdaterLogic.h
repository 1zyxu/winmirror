#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace updater_logic {

inline std::string trimVersion(const std::string& v) {
    size_t start = 0;
    while (start < v.size() && (v[start] == 'v' || v[start] == 'V' || v[start] == ' ')) {
        start++;
    }
    return v.substr(start);
}

inline bool isNewerVersion(const std::string& remote, const std::string& local) {
    std::string r = trimVersion(remote);
    std::string l = trimVersion(local);
    int rMajor = 0, rMinor = 0, rPatch = 0;
    int lMajor = 0, lMinor = 0, lPatch = 0;
    std::sscanf(r.c_str(), "%d.%d.%d", &rMajor, &rMinor, &rPatch);
    std::sscanf(l.c_str(), "%d.%d.%d", &lMajor, &lMinor, &lPatch);
    if (rMajor != lMajor) return rMajor > lMajor;
    if (rMinor != lMinor) return rMinor > lMinor;
    return rPatch > lPatch;
}

inline std::string parseJsonStringField(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    size_t start = json.find('"', pos + needle.size());
    if (start == std::string::npos) return {};
    size_t end = json.find('"', start + 1);
    if (end == std::string::npos) return {};
    return json.substr(start + 1, end - start - 1);
}

inline bool isValidPeHeader(const uint8_t* bytes, size_t len) {
    return bytes && len >= 2 && bytes[0] == 'M' && bytes[1] == 'Z';
}

inline bool isUpdateTempFile(const std::string& filename) {
    std::string lower;
    lower.reserve(filename.size());
    for (unsigned char c : filename) {
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        lower.push_back(static_cast<char>(c));
    }
    auto endsWith = [&](const char* suf) -> bool {
        size_t slen = std::char_traits<char>::length(suf);
        return lower.size() >= slen && lower.compare(lower.size() - slen, slen, suf) == 0;
    };
    return endsWith(".old.exe") ||
           endsWith(".update.exe") ||
           endsWith(".exe.tmp") ||
           endsWith(".exe.bak") ||
           endsWith(".dll.tmp") ||
           endsWith(".dll.update") ||
           endsWith(".pdb") ||
           lower == "winmirror_update.exe" ||
           lower == "vdd_driver.zip";
}

inline std::string findAssetDownloadUrl(const std::string& json, const std::string& assetSubstr) {
    size_t searchPos = 0;
    std::string fallback;
    for (;;) {
        size_t urlPos = json.find("\"browser_download_url\":", searchPos);
        if (urlPos == std::string::npos) break;
        size_t urlStart = json.find('"', urlPos + 23);
        if (urlStart == std::string::npos) break;
        size_t urlEnd = json.find('"', urlStart + 1);
        if (urlEnd == std::string::npos) break;
        std::string cand = json.substr(urlStart + 1, urlEnd - urlStart - 1);
        if (cand.size() > assetSubstr.size() &&
            cand.compare(cand.size() - assetSubstr.size(), assetSubstr.size(), assetSubstr) == 0 &&
            cand[cand.size() - assetSubstr.size() - 1] == '/') {
            return cand;
        }
        if (fallback.empty() && cand.find(assetSubstr) != std::string::npos) {
            fallback = cand;
        }
        searchPos = urlEnd + 1;
    }
    return fallback;
}

inline std::string parseTagFromLocationUrl(const std::string& locationUrl) {
    const std::string marker = "/tag/";
    size_t pos = locationUrl.rfind(marker);
    if (pos == std::string::npos) return {};
    size_t start = pos + marker.size();
    size_t end = start;
    while (end < locationUrl.size() && locationUrl[end] != '/' &&
           locationUrl[end] != '?' && locationUrl[end] != '#' &&
           locationUrl[end] != '\r' && locationUrl[end] != '\n' && locationUrl[end] != ' ') {
        ++end;
    }
    return (end > start) ? locationUrl.substr(start, end - start) : std::string{};
}

} // namespace updater_logic
