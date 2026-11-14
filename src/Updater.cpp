#include "Updater.h"
#include <windows.h>
#include <winhttp.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <sstream>

#pragma comment(lib, "winhttp.lib")

static std::wstring toWide(const std::string& str) {
    if (str.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), NULL, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), &result[0], size);
    return result;
}

static std::string trimVersion(const std::string& v) {
    size_t start = 0;
    while (start < v.size() && (v[start] == 'v' || v[start] == 'V' || v[start] == ' ')) {
        start++;
    }
    return v.substr(start);
}

bool Updater::isNewerVersion(const std::string& remote, const std::string& local) {
    std::string r = trimVersion(remote);
    std::string l = trimVersion(local);

    int rMajor = 0, rMinor = 0, rPatch = 0;
    int lMajor = 0, lMinor = 0, lPatch = 0;

    sscanf(r.c_str(), "%d.%d.%d", &rMajor, &rMinor, &rPatch);
    sscanf(l.c_str(), "%d.%d.%d", &lMajor, &lMinor, &lPatch);

    if (rMajor != lMajor) return rMajor > lMajor;
    if (rMinor != lMinor) return rMinor > lMinor;
    return rPatch > lPatch;
}

bool Updater::fetchLatestReleaseInfo(std::string& outTag, std::string& outDownloadUrl) {
    HINTERNET hSession = WinHttpOpen(
        L"WinMirror-Updater/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (!hSession) return false;

    // Follow redirects automatically
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    HINTERNET hConnect = WinHttpConnect(hSession, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::wstring apiPath = L"/repos/" + toWide(WINMIRROR_REPO) + L"/releases/latest";
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"GET",
        apiPath.c_str(),
        NULL,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE
    );

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::wstring headers = L"User-Agent: WinMirror-Updater\r\nAccept: application/vnd.github.v3+json\r\n";
    BOOL bResults = WinHttpSendRequest(
        hRequest,
        headers.c_str(),
        static_cast<DWORD>(headers.length()),
        WINHTTP_NO_REQUEST_DATA,
        0,
        0,
        0
    );

    if (bResults) {
        bResults = WinHttpReceiveResponse(hRequest, NULL);
    }

    std::string response;
    if (bResults) {
        DWORD dwSize = 0;
        do {
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
            if (dwSize == 0) break;

            std::vector<char> buf(dwSize);
            DWORD dwDownloaded = 0;
            if (WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded)) {
                response.append(buf.data(), dwDownloaded);
            }
        } while (dwSize > 0);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (response.empty()) return false;

    // Parse tag_name
    size_t tagPos = response.find("\"tag_name\":");
    if (tagPos == std::string::npos) return false;
    size_t tagStart = response.find('"', tagPos + 11);
    if (tagStart == std::string::npos) return false;
    size_t tagEnd = response.find('"', tagStart + 1);
    if (tagEnd == std::string::npos) return false;
    outTag = response.substr(tagStart + 1, tagEnd - tagStart - 1);

    // Parse browser_download_url for winmirror.exe
    size_t exePos = response.find("winmirror.exe");
    if (exePos != std::string::npos) {
        size_t urlPos = response.rfind("\"browser_download_url\":", exePos);
        if (urlPos != std::string::npos) {
            size_t urlStart = response.find('"', urlPos + 23);
            if (urlStart != std::string::npos) {
                size_t urlEnd = response.find('"', urlStart + 1);
                if (urlEnd != std::string::npos) {
                    outDownloadUrl = response.substr(urlStart + 1, urlEnd - urlStart - 1);
                }
            }
        }
    }

    return !outTag.empty();
}

bool Updater::downloadFile(const std::string& url, const std::string& outputPath) {
    // Parse URL into host and path
    URL_COMPONENTS urlComp = {};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t hostName[256] = {};
    wchar_t urlPath[2048] = {};
    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = sizeof(hostName) / sizeof(hostName[0]);
    urlComp.lpszUrlPath = urlPath;
    urlComp.dwUrlPathLength = sizeof(urlPath) / sizeof(urlPath[0]);

    std::wstring wUrl = toWide(url);
    if (!WinHttpCrackUrl(wUrl.c_str(), static_cast<DWORD>(wUrl.length()), 0, &urlComp)) {
        return false;
    }

    HINTERNET hSession = WinHttpOpen(
        L"WinMirror-Updater/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (!hSession) return false;

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::wstring headers = L"User-Agent: WinMirror-Updater\r\n";
    BOOL bResults = WinHttpSendRequest(hRequest, headers.c_str(), static_cast<DWORD>(headers.length()), WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (bResults) {
        bResults = WinHttpReceiveResponse(hRequest, NULL);
    }

    if (!bResults) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::ofstream outFile(outputPath, std::ios::binary);
    if (!outFile.is_open()) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD dwSize = 0;
    size_t totalBytes = 0;
    do {
        dwSize = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
        if (dwSize == 0) break;

        std::vector<char> buf(dwSize);
        DWORD dwDownloaded = 0;
        if (WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded)) {
            outFile.write(buf.data(), dwDownloaded);
            totalBytes += dwDownloaded;
        }
    } while (dwSize > 0);

    outFile.close();
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return totalBytes > 1024;
}

void Updater::applyUpdateAndRestart(const std::string& newExePath) {
    char currentExe[MAX_PATH] = {};
    GetModuleFileNameA(NULL, currentExe, MAX_PATH);

    std::ostringstream cmd;
    cmd << "cmd.exe /c \"ping 127.0.0.1 -n 2 > nul & "
        << "move /y \"" << newExePath << "\" \"" << currentExe << "\" & "
        << "start \"\" \"" << currentExe << "\"\"";

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::string cmdStr = cmd.str();

    std::cout << "[Updater] Applying update and restarting..." << std::endl;
    if (CreateProcessA(NULL, &cmdStr[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        exit(0);
    }
}

bool Updater::checkForUpdates(bool autoInstall) {
    std::cout << "[Updater] Checking for updates on GitHub (" << WINMIRROR_REPO << ")..." << std::endl;
    std::string latestTag, downloadUrl;
    if (!fetchLatestReleaseInfo(latestTag, downloadUrl)) {
        std::cout << "[Updater] Could not reach GitHub releases or repository is private." << std::endl;
        return false;
    }

    std::cout << "[Updater] Current version: v" << WINMIRROR_VERSION
              << " | Latest release: " << latestTag << std::endl;

    if (!isNewerVersion(latestTag, WINMIRROR_VERSION)) {
        std::cout << "[Updater] WinMirror is already up to date!" << std::endl;
        return false;
    }

    std::cout << "[Updater] *** New update available: " << latestTag << "! ***" << std::endl;
    if (!autoInstall || downloadUrl.empty()) {
        std::cout << "[Updater] Download link: " << downloadUrl << std::endl;
        return true;
    }

    std::cout << "[Updater] Downloading update from " << downloadUrl << "..." << std::endl;
    std::string tempExe = "winmirror_update.exe";
    if (downloadFile(downloadUrl, tempExe)) {
        std::cout << "[Updater] Update downloaded successfully (" << tempExe << ")." << std::endl;
        applyUpdateAndRestart(tempExe);
        return true;
    } else {
        std::cerr << "[Updater] Failed to download update." << std::endl;
        return false;
    }
}
