#pragma once

#include "Common.h"
#include <string>

class Updater {
public:
    static bool checkForUpdates(bool autoInstall = true);
    static void applyUpdateAndRestart(const std::string& newExePath);

private:
    static bool fetchLatestReleaseInfo(std::string& outTag, std::string& outDownloadUrl);
    static bool downloadFile(const std::string& url, const std::string& outputPath);
    static bool isNewerVersion(const std::string& remoteVersion, const std::string& localVersion);
};
