#pragma once

#include "Common.h"
#include <string>

class Updater {
public:
    static bool checkForUpdates(bool autoInstall = true);
    static bool ensureCompanionDll(const std::wstring& exeDir);
    static bool downloadVirtualDisplayPackage(const std::wstring& outputZipPath);
    static void applyUpdateAndRestart(const std::string& newExePath);
    static std::wstring getExecutableDir();

private:
    static bool fetchReleaseJson(const std::string& repo, const std::string& tagOrLatest, std::string& outJson);
    static std::string parseJsonStringField(const std::string& json, const std::string& key);
    static std::string findAssetDownloadUrl(const std::string& json, const std::string& assetSubstr);
    static bool downloadFileW(const std::string& url, const std::wstring& outputPath);
    static bool isNewerVersion(const std::string& remoteVersion, const std::string& localVersion);
};
