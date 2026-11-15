#include "Updater.h"
#include "UpdaterLogic.h"
#include "WinMirrorRuntime.h"
#include <windows.h>
#include <winhttp.h>
#include <iostream>
#include <vector>
#include <cstdio>

#pragma comment(lib, "winhttp.lib")

namespace {

std::wstring toWide(const std::string& str) {
    if (str.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), &result[0], size);
    return result;
}

std::string toUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), &result[0], size, nullptr, nullptr);
    return result;
}

using updater_logic::trimVersion;

bool openWinHttpGet(const std::wstring& host, INTERNET_PORT port, const std::wstring& path, bool https,
                    HINTERNET& outSession, HINTERNET& outConnect, HINTERNET& outRequest, const wchar_t* extraHeaders) {
    outSession = WinHttpOpen(L"WinMirror-Updater/2.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!outSession) return false;

    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(outSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    WinHttpSetTimeouts(outSession, 2500, 2500, 3000, 12000);

    outConnect = WinHttpConnect(outSession, host.c_str(), port, 0);
    if (!outConnect) { WinHttpCloseHandle(outSession); outSession = nullptr; return false; }

    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    outRequest = WinHttpOpenRequest(outConnect, L"GET", path.c_str(), nullptr,
                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!outRequest) {
        WinHttpCloseHandle(outConnect); outConnect = nullptr;
        WinHttpCloseHandle(outSession); outSession = nullptr;
        return false;
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(outRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    std::wstring headers = extraHeaders ? std::wstring(extraHeaders) : L"User-Agent: WinMirror-Updater\r\n";
    BOOL ok = WinHttpSendRequest(outRequest, headers.c_str(), static_cast<DWORD>(headers.length()),
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(outRequest, nullptr);

    DWORD statusCode = 0, statusSize = sizeof(statusCode);
    if (!ok ||
        !WinHttpQueryHeaders(outRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX) ||
        statusCode != 200) {
        WinHttpCloseHandle(outRequest); outRequest = nullptr;
        WinHttpCloseHandle(outConnect); outConnect = nullptr;
        WinHttpCloseHandle(outSession); outSession = nullptr;
        return false;
    }
    return true;
}

void closeWinHttp(HINTERNET& hSession, HINTERNET& hConnect, HINTERNET& hRequest) {
    if (hRequest) { WinHttpCloseHandle(hRequest); hRequest = nullptr; }
    if (hConnect) { WinHttpCloseHandle(hConnect); hConnect = nullptr; }
    if (hSession) { WinHttpCloseHandle(hSession); hSession = nullptr; }
}

} // namespace

std::wstring Updater::getExecutableDir() {
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return L".";
    std::wstring s(path);
    size_t slash = s.find_last_of(L"\\/");
    return (slash != std::wstring::npos) ? s.substr(0, slash) : L".";
}

bool Updater::isNewerVersion(const std::string& remote, const std::string& local) {
    return updater_logic::isNewerVersion(remote, local);
}

bool Updater::fetchReleaseJson(const std::string& repo, const std::string& tagOrLatest, std::string& outJson) {
    outJson.clear();
    std::wstring apiPath = L"/repos/" + toWide(repo) + L"/releases/" +
                           ((tagOrLatest == "latest") ? L"latest" : (L"tags/" + toWide(tagOrLatest)));

    HINTERNET hSession = nullptr, hConnect = nullptr, hRequest = nullptr;
    if (!openWinHttpGet(L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, apiPath, true,
                        hSession, hConnect, hRequest,
                        L"User-Agent: WinMirror-Updater\r\nAccept: application/vnd.github.v3+json\r\n")) {
        return false;
    }

    DWORD dwSize = 0;
    do {
        dwSize = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;
        std::vector<char> buf(dwSize);
        DWORD dwDownloaded = 0;
        if (WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded) && dwDownloaded > 0) {
            outJson.append(buf.data(), dwDownloaded);
        }
    } while (dwSize > 0);

    closeWinHttp(hSession, hConnect, hRequest);
    return !outJson.empty();
}

std::string Updater::parseJsonStringField(const std::string& json, const std::string& key) {
    return updater_logic::parseJsonStringField(json, key);
}

bool Updater::isValidPeHeader(const uint8_t* bytes, size_t len) {
    return updater_logic::isValidPeHeader(bytes, len);
}

std::string Updater::findAssetDownloadUrl(const std::string& json, const std::string& assetSubstr) {
    return updater_logic::findAssetDownloadUrl(json, assetSubstr);
}

bool Updater::downloadFileW(const std::string& url, const std::wstring& outputPath, bool verifyPeHeader) {
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

    HINTERNET hSession = nullptr, hConnect = nullptr, hRequest = nullptr;
    if (!openWinHttpGet(hostName, urlComp.nPort, urlPath, urlComp.nScheme == INTERNET_SCHEME_HTTPS,
                        hSession, hConnect, hRequest, L"User-Agent: WinMirror-Updater\r\n")) {
        return false;
    }

    HANDLE hFile = CreateFileW(outputPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        closeWinHttp(hSession, hConnect, hRequest);
        return false;
    }

    DWORD dwSize = 0;
    size_t totalBytes = 0;
    uint8_t peMagic[2] = {};
    size_t peHave = 0;
    bool writeOk = true;
    do {
        dwSize = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;
        std::vector<char> buf(dwSize);
        DWORD dwDownloaded = 0;
        if (WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded) && dwDownloaded > 0) {
            for (DWORD k = 0; k < dwDownloaded && peHave < 2; ++k) {
                peMagic[peHave++] = static_cast<uint8_t>(buf[k]);
            }
            if (verifyPeHeader && peHave == 2 && !isValidPeHeader(peMagic, peHave)) {
                writeOk = false;
                break;
            }
            DWORD written = 0;
            if (!WriteFile(hFile, buf.data(), dwDownloaded, &written, nullptr) || written != dwDownloaded) {
                writeOk = false;
                break;
            }
            totalBytes += dwDownloaded;
        }
    } while (dwSize > 0);

    CloseHandle(hFile);
    closeWinHttp(hSession, hConnect, hRequest);
    const size_t minBytes = verifyPeHeader ? 4096u : 1024u;
    if (!writeOk || totalBytes <= minBytes || (verifyPeHeader && !isValidPeHeader(peMagic, peHave))) {
        DeleteFileW(outputPath.c_str());
        return false;
    }
    return true;
}

bool Updater::ensureCompanionDll(const std::wstring& exeDir) {
    std::wstring dllPath = exeDir + L"\\winmirror_rt.dll";
    std::string loadedVer;
    if (WinMirrorRuntimeApi::probeDllVersion(dllPath, loadedVer) && loadedVer == WINMIRROR_VERSION) {
        return true;
    }

    std::cout << "[Updater] Runtime DLL (" << toUtf8(dllPath) << ") is "
              << (loadedVer.empty() ? "missing" : ("v" + loadedVer + " != v" + WINMIRROR_VERSION))
              << "; installing winmirror_rt.dll v" << WINMIRROR_VERSION << "..." << std::endl;

    std::string dllUrl;
    std::string json;
    std::string exactTag = std::string("v") + WINMIRROR_VERSION;
    if (fetchReleaseJson(WINMIRROR_REPO, exactTag, json)) {
        dllUrl = findAssetDownloadUrl(json, "winmirror_rt.dll");
    }
    if (dllUrl.empty() && fetchReleaseJson(WINMIRROR_REPO, "latest", json)) {
        std::string latestTag = parseJsonStringField(json, "tag_name");
        if (trimVersion(latestTag) == WINMIRROR_VERSION) {
            dllUrl = findAssetDownloadUrl(json, "winmirror_rt.dll");
        }
    }
    if (dllUrl.empty()) {
        dllUrl = "https://github.com/" + std::string(WINMIRROR_REPO) +
                 "/releases/download/" + exactTag + "/winmirror_rt.dll";
    }

    std::wstring tmpDll = dllPath + L".tmp";
    if (!downloadFileW(dllUrl, tmpDll, true)) {
        std::cerr << "[Updater] Could not download winmirror_rt.dll yet (continuing with built-in fallback)." << std::endl;
        return false;
    }

    std::string verifiedVer;
    if (!WinMirrorRuntimeApi::probeDllVersion(tmpDll, verifiedVer) || verifiedVer != WINMIRROR_VERSION) {
        DeleteFileW(tmpDll.c_str());
        std::cerr << "[Updater] Downloaded winmirror_rt.dll version mismatch (" << verifiedVer << ")." << std::endl;
        return false;
    }

    if (!MoveFileExW(tmpDll.c_str(), dllPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmpDll.c_str());
        return false;
    }
    std::cout << "[Updater] Installed winmirror_rt.dll v" << verifiedVer << " cleanly." << std::endl;
    return true;
}

bool Updater::downloadVirtualDisplayPackage(const std::wstring& outputZipPath) {
    std::string json;
    if (!fetchReleaseJson("VirtualDrivers/Virtual-Display-Driver", "latest", json)) {
        return false;
    }
    std::string zipUrl = findAssetDownloadUrl(json, "VDD.Control");
    if (zipUrl.empty()) {
        zipUrl = findAssetDownloadUrl(json, "VirtualDisplayDriver-x86.Driver.Only");
    }
    if (zipUrl.empty()) return false;
    std::cout << "[Driver] Downloading Virtual Display Driver from " << zipUrl << "..." << std::endl;
    return downloadFileW(zipUrl, outputZipPath);
}

void Updater::applyUpdateAndRestart(const std::string& newExePath) {
    wchar_t currentExe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, currentExe, MAX_PATH) == 0) return;

    std::wstring newExeW = toWide(newExePath);
    std::wstring oldExe = std::wstring(currentExe) + L".old.exe";
    if (!MoveFileExW(currentExe, oldExe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return;
    }
    if (!MoveFileExW(newExeW.c_str(), currentExe, MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(oldExe.c_str(), currentExe, MOVEFILE_REPLACE_EXISTING);
        return;
    }

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::cout << "[Updater] Applying update and restarting..." << std::endl;
    if (CreateProcessW(currentExe, nullptr, nullptr, nullptr, FALSE,
                       DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        exit(0);
    }
}

bool Updater::checkForUpdates(bool autoInstall) {
    std::cout << "[Updater] Checking for updates on GitHub (" << WINMIRROR_REPO << ")..." << std::endl;
    std::string json;
    if (!fetchReleaseJson(WINMIRROR_REPO, "latest", json)) {
        std::cout << "[Updater] Could not reach GitHub releases or repository is private." << std::endl;
        return false;
    }

    std::string latestTag = parseJsonStringField(json, "tag_name");
    std::string exeUrl = findAssetDownloadUrl(json, "winmirror.exe");
    std::string dllUrl = findAssetDownloadUrl(json, "winmirror_rt.dll");
    if (exeUrl.empty() && !latestTag.empty()) {
        exeUrl = "https://github.com/" + std::string(WINMIRROR_REPO) +
                 "/releases/download/" + latestTag + "/winmirror.exe";
    }

    std::cout << "[Updater] Current version: v" << WINMIRROR_VERSION
              << " | Latest release: " << latestTag << std::endl;

    if (!isNewerVersion(latestTag, WINMIRROR_VERSION)) {
        std::cout << "[Updater] WinMirror is already up to date!" << std::endl;
        return false;
    }

    std::cout << "[Updater] *** New update available: " << latestTag << "! ***" << std::endl;
    if (!autoInstall || exeUrl.empty()) {
        std::cout << "[Updater] Download link: " << exeUrl << std::endl;
        return true;
    }

    if (!dllUrl.empty()) {
        std::wstring dllDest = getExecutableDir() + L"\\winmirror_rt.dll";
        std::wstring dllTmp = dllDest + L".update";
        WinMirrorRuntimeApi::instance().unload();
        if (downloadFileW(dllUrl, dllTmp, true)) {
            if (!MoveFileExW(dllTmp.c_str(), dllDest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                DeleteFileW(dllTmp.c_str());
            } else {
                std::cout << "[Updater] Updated winmirror_rt.dll for " << latestTag << "." << std::endl;
            }
        }
    }

    wchar_t currentExeW[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, currentExeW, MAX_PATH);
    std::wstring tempExeW = std::wstring(currentExeW) + L".update.exe";
    std::string tempExe = toUtf8(tempExeW);

    std::cout << "[Updater] Downloading update from " << exeUrl << "..." << std::endl;
    if (downloadFileW(exeUrl, tempExeW, true)) {
        std::cout << "[Updater] Update downloaded successfully (" << tempExe << ")." << std::endl;
        applyUpdateAndRestart(tempExe);
        return true;
    } else {
        DeleteFileW(tempExeW.c_str());
        std::cerr << "[Updater] Failed to download update." << std::endl;
        return false;
    }
}
