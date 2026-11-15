#pragma once

#include "Common.h"
#include <functional>
#include <string>

class Updater {
public:
    using StatusCallback = std::function<void(const std::string&)>;

    static bool checkForUpdates(bool autoInstall = true, StatusCallback onStatus = nullptr);
    static bool ensureCompanionDll(const std::wstring& exeDir);
    static bool downloadVirtualDisplayPackage(const std::wstring& outputZipPath);
    static void applyUpdateAndRestart(const std::string& newExePath);
    static std::wstring getExecutableDir();

    static bool isNewerVersion(const std::string& remoteVersion, const std::string& localVersion);
    static std::string parseJsonStringField(const std::string& json, const std::string& key);
    static std::string findAssetDownloadUrl(const std::string& json, const std::string& assetSubstr);
    static bool isValidPeHeader(const uint8_t* bytes, size_t len);

private:
    static bool fetchReleaseJson(const std::string& repo, const std::string& tagOrLatest, std::string& outJson);
    static bool downloadFileW(const std::string& url, const std::wstring& outputPath, bool verifyPeHeader = false);
};
