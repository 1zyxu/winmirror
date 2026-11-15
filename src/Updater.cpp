#include "Updater.h"
#include "UpdaterLogic.h"
#include "SecProtect.h"
#include "WinMirrorRuntime.h"
#include <windows.h>
#include <winhttp.h>
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

std::string getUpdaterRepoSlug() {
    return WM_OBF("1zyxu/winmirror");
}

bool openWinHttpGet(const std::wstring& host, INTERNET_PORT port, const std::wstring& path, bool https,
                    HINTERNET& outSession, HINTERNET& outConnect, HINTERNET& outRequest, const std::wstring& extraHeaders) {
    std::wstring ua = WM_OBF_W(L"WM-Client/2.5");
    outSession = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    wm_sec::wipeWString(ua);
    if (!outSession) return false;

    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(outSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    WinHttpSetTimeouts(outSession, 1500, 2000, 2500, 10000);

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

    std::wstring headers = !extraHeaders.empty() ? extraHeaders : WM_OBF_W(L"User-Agent: WM-Client\r\n");
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
    wm_sec::enforceAntiReverse();
    outJson.clear();
    std::wstring pPrefix = WM_OBF_W(L"/repos/");
    std::wstring pRel = WM_OBF_W(L"/releases/");
    std::wstring pLatest = WM_OBF_W(L"latest");
    std::wstring pTags = WM_OBF_W(L"tags/");
    std::wstring apiHost = WM_OBF_W(L"api.github.com");
    std::wstring apiHdr = WM_OBF_W(L"User-Agent: WM-Client\r\nAccept: application/vnd.github.v3+json\r\n");

    std::wstring apiPath = pPrefix + toWide(repo) + pRel +
                           ((tagOrLatest == toUtf8(pLatest)) ? pLatest : (pTags + toWide(tagOrLatest)));

    HINTERNET hSession = nullptr, hConnect = nullptr, hRequest = nullptr;
    bool openOk = openWinHttpGet(apiHost, INTERNET_DEFAULT_HTTPS_PORT, apiPath, true,
                                 hSession, hConnect, hRequest, apiHdr);
    wm_sec::wipeWString(pPrefix);
    wm_sec::wipeWString(pRel);
    wm_sec::wipeWString(pLatest);
    wm_sec::wipeWString(pTags);
    wm_sec::wipeWString(apiHost);
    wm_sec::wipeWString(apiHdr);
    wm_sec::wipeWString(apiPath);
    if (!openOk) return false;

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
    wm_sec::enforceAntiReverse();
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
                        hSession, hConnect, hRequest, L"")) {
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
    wm_sec::enforceAntiReverse();
    std::wstring dllNameW = WM_OBF_W(L"\\winmirror_rt.dll");
    std::wstring dllPath = exeDir + dllNameW;
    wm_sec::wipeWString(dllNameW);

    std::string loadedVer;
    if (WinMirrorRuntimeApi::probeDllVersion(dllPath, loadedVer) && loadedVer == WINMIRROR_VERSION) {
        return true;
    }

    std::string repo = getUpdaterRepoSlug();
    std::string dllAsset = WM_OBF("winmirror_rt.dll");
    std::string tagKey = WM_OBF("tag_name");
    std::string latestKey = WM_OBF("latest");
    std::string exactTag = std::string("v") + WINMIRROR_VERSION;

    std::string dllUrl;
    std::string json;
    if (fetchReleaseJson(repo, exactTag, json)) {
        dllUrl = findAssetDownloadUrl(json, dllAsset);
    }
    if (dllUrl.empty() && fetchReleaseJson(repo, latestKey, json)) {
        std::string latestTag = parseJsonStringField(json, tagKey);
        if (trimVersion(latestTag) == WINMIRROR_VERSION) {
            dllUrl = findAssetDownloadUrl(json, dllAsset);
        }
    }
    if (dllUrl.empty()) {
        std::string ghBase = WM_OBF("https://github.com/");
        std::string dlPath = WM_OBF("/releases/download/");
        dllUrl = ghBase + repo + dlPath + exactTag + "/" + dllAsset;
        wm_sec::wipeString(ghBase);
        wm_sec::wipeString(dlPath);
    }
    wm_sec::wipeString(repo);
    wm_sec::wipeString(tagKey);
    wm_sec::wipeString(latestKey);

    std::wstring tmpSuffix = WM_OBF_W(L".tmp");
    std::wstring tmpDll = dllPath + tmpSuffix;
    wm_sec::wipeWString(tmpSuffix);
    if (!downloadFileW(dllUrl, tmpDll, true)) {
        wm_sec::wipeString(dllAsset);
        return false;
    }
    wm_sec::wipeString(dllAsset);

    std::string verifiedVer;
    if (!WinMirrorRuntimeApi::probeDllVersion(tmpDll, verifiedVer) || verifiedVer != WINMIRROR_VERSION) {
        DeleteFileW(tmpDll.c_str());
        return false;
    }

    if (!MoveFileExW(tmpDll.c_str(), dllPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmpDll.c_str());
        return false;
    }
    return true;
}

bool Updater::downloadVirtualDisplayPackage(const std::wstring& outputZipPath) {
    std::string json;
    std::string vddRepo = WM_OBF("VirtualDrivers/Virtual-Display-Driver");
    std::string latestKey = WM_OBF("latest");
    bool fetched = fetchReleaseJson(vddRepo, latestKey, json);
    wm_sec::wipeString(vddRepo);
    wm_sec::wipeString(latestKey);
    if (!fetched) return false;

    std::string a1 = WM_OBF("VDD.Control");
    std::string a2 = WM_OBF("VirtualDisplayDriver-x86.Driver.Only");
    std::string zipUrl = findAssetDownloadUrl(json, a1);
    if (zipUrl.empty()) zipUrl = findAssetDownloadUrl(json, a2);
    wm_sec::wipeString(a1);
    wm_sec::wipeString(a2);
    if (zipUrl.empty()) return false;
    return downloadFileW(zipUrl, outputZipPath);
}

void Updater::applyUpdateAndRestart(const std::string& newExePath) {
    wm_sec::enforceAntiReverse();
    wchar_t currentExe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, currentExe, MAX_PATH) == 0) return;

    std::wstring newExeW = toWide(newExePath);
    std::wstring oldSuf = WM_OBF_W(L".old.exe");
    std::wstring oldExe = std::wstring(currentExe) + oldSuf;
    wm_sec::wipeWString(oldSuf);

    if (!MoveFileExW(currentExe, oldExe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(newExeW.c_str());
        return;
    }
    if (!MoveFileExW(newExeW.c_str(), currentExe, MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(oldExe.c_str(), currentExe, MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(newExeW.c_str());
        return;
    }

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(currentExe, nullptr, nullptr, nullptr, FALSE,
                       DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        ExitProcess(0);
    }
}

bool Updater::checkForUpdates(bool autoInstall, StatusCallback onStatus) {
    wm_sec::enforceAntiReverse();
    wm_sec::hideCurrentThread();
    if (onStatus) onStatus("Checking for updates...");

    std::string repo = getUpdaterRepoSlug();
    std::string latestKey = WM_OBF("latest");
    std::string json;
    bool fetched = fetchReleaseJson(repo, latestKey, json);
    wm_sec::wipeString(latestKey);
    if (!fetched) {
        wm_sec::wipeString(repo);
        return false;
    }

    std::string tagKey = WM_OBF("tag_name");
    std::string exeAsset = WM_OBF("winmirror.exe");
    std::string dllAsset = WM_OBF("winmirror_rt.dll");
    std::string latestTag = parseJsonStringField(json, tagKey);
    std::string exeUrl = findAssetDownloadUrl(json, exeAsset);
    std::string dllUrl = findAssetDownloadUrl(json, dllAsset);
    wm_sec::wipeString(tagKey);

    if (exeUrl.empty() && !latestTag.empty()) {
        std::string ghBase = WM_OBF("https://github.com/");
        std::string dlPath = WM_OBF("/releases/download/");
        exeUrl = ghBase + repo + dlPath + latestTag + "/" + exeAsset;
        wm_sec::wipeString(ghBase);
        wm_sec::wipeString(dlPath);
    }
    wm_sec::wipeString(repo);
    wm_sec::wipeString(exeAsset);

    if (!isNewerVersion(latestTag, WINMIRROR_VERSION)) {
        wm_sec::wipeString(dllAsset);
        return false;
    }

    if (!autoInstall || exeUrl.empty()) {
        wm_sec::wipeString(dllAsset);
        return true;
    }

    if (onStatus) onStatus("Downloading update " + latestTag + "...");

    if (!dllUrl.empty()) {
        std::wstring updSuf = WM_OBF_W(L".update");
        std::wstring dllDest = getExecutableDir() + L"\\" + toWide(dllAsset);
        std::wstring dllTmp = dllDest + updSuf;
        wm_sec::wipeWString(updSuf);
        WinMirrorRuntimeApi::instance().unload();
        if (downloadFileW(dllUrl, dllTmp, true)) {
            if (!MoveFileExW(dllTmp.c_str(), dllDest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                DeleteFileW(dllTmp.c_str());
            }
        }
    }
    wm_sec::wipeString(dllAsset);

    wchar_t currentExeW[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, currentExeW, MAX_PATH);
    std::wstring updExeSuf = WM_OBF_W(L".update.exe");
    std::wstring tempExeW = std::wstring(currentExeW) + updExeSuf;
    wm_sec::wipeWString(updExeSuf);
    std::string tempExe = toUtf8(tempExeW);

    if (downloadFileW(exeUrl, tempExeW, true)) {
        if (onStatus) onStatus("Installing " + latestTag + " & restarting...");
        applyUpdateAndRestart(tempExe);
        return true;
    } else {
        DeleteFileW(tempExeW.c_str());
        return false;
    }
}
