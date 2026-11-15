#include "BluetoothBridge.h"
#include <ws2bth.h>
#include <bluetoothapis.h>
#include <iostream>
#include <sstream>

static std::string wideToUtf8(const wchar_t* wstr) {
    if (!wstr || !wstr[0]) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return "";
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &out[0], len, nullptr, nullptr);
    return out;
}

BluetoothBridge::BluetoothBridge(int mediaPort, std::function<void()> onKeyframeReq)
    : m_mediaPort(mediaPort), m_onKeyframeReq(std::move(onKeyframeReq)) {}

BluetoothBridge::~BluetoothBridge() {
    stop();
}

void BluetoothBridge::detectPairedDevices() {
    BLUETOOTH_FIND_RADIO_PARAMS radioParams = { sizeof(radioParams) };
    HANDLE hRadio = NULL;
    HBLUETOOTH_RADIO_FIND hFindRadio = BluetoothFindFirstRadio(&radioParams, &hRadio);
    if (!hFindRadio) {
        m_radioAvailable = false;
        std::lock_guard<std::mutex> lock(m_metaMutex);
        m_radioName = "No BT Radio";
        return;
    }

    m_radioAvailable = true;
    BLUETOOTH_RADIO_INFO radioInfo = { sizeof(radioInfo) };
    if (BluetoothGetRadioInfo(hRadio, &radioInfo) == ERROR_SUCCESS) {
        std::lock_guard<std::mutex> lock(m_metaMutex);
        m_radioName = wideToUtf8(radioInfo.szName);
    }
    if (!BluetoothIsConnectable(hRadio)) {
        BluetoothEnableIncomingConnections(hRadio, TRUE);
    }
    CloseHandle(hRadio);
    BluetoothFindRadioClose(hFindRadio);

    BLUETOOTH_DEVICE_SEARCH_PARAMS searchParams = { sizeof(searchParams) };
    searchParams.fReturnAuthenticated = TRUE;
    searchParams.fReturnRemembered = TRUE;
    searchParams.fReturnConnected = TRUE;
    searchParams.fIssueInquiry = FALSE;

    BLUETOOTH_DEVICE_INFO devInfo = { sizeof(devInfo) };
    HBLUETOOTH_DEVICE_FIND hFindDev = BluetoothFindFirstDevice(&searchParams, &devInfo);
    if (hFindDev) {
        std::vector<std::string> names;
        do {
            std::string n = wideToUtf8(devInfo.szName);
            if (!n.empty()) names.push_back(n);
        } while (BluetoothFindNextDevice(hFindDev, &devInfo));
        BluetoothFindDeviceClose(hFindDev);

        std::ostringstream ss;
        for (size_t i = 0; i < names.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << names[i];
        }
        std::lock_guard<std::mutex> lock(m_metaMutex);
        m_pairedNames = ss.str();
    }
}

bool BluetoothBridge::start(const std::string& ipsCsv, uint32_t width, uint32_t height, bool isHdr, const std::string& codecName) {
    updateMetadata(ipsCsv, width, height, isHdr, codecName);
    detectPairedDevices();

    if (!m_radioAvailable) {
        return false;
    }

    if (!m_wsaStarted) {
        WSADATA wsaData = {};
        m_wsaStarted = (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);
    }

    SOCKET listenSock = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (listenSock == INVALID_SOCKET) {
        m_radioAvailable = false;
        return false;
    }

    SOCKADDR_BTH sa = {};
    sa.addressFamily = AF_BTH;
    sa.port = BT_PORT_ANY;

    if (bind(listenSock, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == SOCKET_ERROR) {
        closesocket(listenSock);
        return false;
    }

    int saLen = sizeof(sa);
    if (getsockname(listenSock, reinterpret_cast<sockaddr*>(&sa), &saLen) == SOCKET_ERROR) {
        closesocket(listenSock);
        return false;
    }

    // Publish Windows Bluetooth SDP Record before listen() (NS_BTH requires Unicode WSAQUERYSETW)
    GUID serviceUuid = WINMIRROR_BT_SERVICE_UUID;
    wchar_t svcName[] = L"WinMirror AirPlay BT";
    wchar_t svcComment[] = L"WinMirror AirPlay Bluetooth Discovery & Control";

    auto tryRegisterSdp = [&](bool setRemoteAddr) -> int {
        CSADDR_INFO csAddr = {};
        csAddr.LocalAddr.lpSockaddr = reinterpret_cast<LPSOCKADDR>(&sa);
        csAddr.LocalAddr.iSockaddrLength = sizeof(sa);
        if (setRemoteAddr) {
            csAddr.RemoteAddr.lpSockaddr = reinterpret_cast<LPSOCKADDR>(&sa);
            csAddr.RemoteAddr.iSockaddrLength = sizeof(sa);
        }
        csAddr.iSocketType = SOCK_STREAM;
        csAddr.iProtocol = BTHPROTO_RFCOMM;

        WSAQUERYSETW wsaq = {};
        wsaq.dwSize = sizeof(wsaq);
        wsaq.lpszServiceInstanceName = svcName;
        wsaq.lpServiceClassId = &serviceUuid;
        wsaq.lpszComment = svcComment;
        wsaq.dwNameSpace = NS_BTH;
        wsaq.dwNumberOfCsAddrs = 1;
        wsaq.lpcsaBuffer = &csAddr;
        return (WSASetServiceW(&wsaq, RNRSERVICE_REGISTER, 0) == SOCKET_ERROR) ? WSAGetLastError() : 0;
    };

    int sdpErr = tryRegisterSdp(false);
    if (sdpErr != 0) {
        sdpErr = tryRegisterSdp(true);
    }

    if (listen(listenSock, 4) == SOCKET_ERROR) {
        closesocket(listenSock);
        return false;
    }

    m_listenSock.store(listenSock);

    std::cout << "[Bluetooth] Radio: " << m_radioName
              << " | RFCOMM Ch " << sa.port
              << (sdpErr == 0 ? " | SDP=OK" : (" | SDP=err(" + std::to_string(sdpErr) + ")"));
    if (!m_pairedNames.empty()) {
        std::cout << " | Paired: " << m_pairedNames;
    }
    std::cout << std::endl;

    m_running = true;
    m_thread = std::thread(&BluetoothBridge::serverLoop, this);
    return true;
}

void BluetoothBridge::stop() {
    m_running = false;
    m_btConnected = false;
    SOCKET ls = m_listenSock.exchange(INVALID_SOCKET);
    if (ls != INVALID_SOCKET) {
        closesocket(ls);
    }
    {
        std::lock_guard<std::mutex> lock(m_clientMutex);
        if (m_clientSock != INVALID_SOCKET) {
            shutdown(m_clientSock, SD_BOTH);
            closesocket(m_clientSock);
            m_clientSock = INVALID_SOCKET;
        }
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_wsaStarted) {
        WSACleanup();
        m_wsaStarted = false;
    }
}

void BluetoothBridge::sendToClient(const std::string& line) {
    if (line.empty()) return;
    std::lock_guard<std::mutex> lock(m_clientMutex);
    if (m_clientSock != INVALID_SOCKET) {
        send(m_clientSock, line.c_str(), static_cast<int>(line.size()), 0);
    }
}

void BluetoothBridge::updateMetadata(const std::string& ipsCsv, uint32_t width, uint32_t height, bool isHdr, const std::string& codecName) {
    bool ipsChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_metaMutex);
        ipsChanged = (m_ipsCsv != ipsCsv);
        m_ipsCsv = ipsCsv;
        m_width = width;
        m_height = height;
        m_isHdr = isHdr;
        m_codecName = codecName;
    }
    if (ipsChanged && m_btConnected.load()) {
        sendToClient(buildHelloLine());
    }
}

std::string BluetoothBridge::buildHelloLine() const {
    char host[128] = "WindowsPC";
    gethostname(host, sizeof(host));
    host[sizeof(host) - 1] = '\0';

    std::lock_guard<std::mutex> lock(m_metaMutex);
    std::ostringstream ss;
    ss << "WINMIRROR_BT_HELLO|host=" << host
       << "|port=" << m_mediaPort
       << "|ips=" << m_ipsCsv
       << "|res=" << m_width << "x" << m_height
       << "|hdr=" << (m_isHdr ? 1 : 0)
       << "|codec=" << m_codecName << "\n";
    return ss.str();
}

std::string BluetoothBridge::getStatusText() const {
    if (!m_radioAvailable) return "Off";
    if (m_btConnected) return "Linked";
    return "Ready";
}

void BluetoothBridge::serverLoop() {
    while (m_running) {
        SOCKET ls = m_listenSock.load();
        if (ls == INVALID_SOCKET) break;
        SOCKADDR_BTH clientAddr = {};
        int addrLen = sizeof(clientAddr);
        SOCKET clientSock = accept(ls, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
        if (clientSock == INVALID_SOCKET) {
            if (!m_running) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(m_clientMutex);
            m_clientSock = clientSock;
        }
        m_btConnected = true;
        sendToClient(buildHelloLine());

        char buf[512];
        while (m_running) {
            int n = recv(clientSock, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            buf[n] = '\0';
            std::string cmd(buf, n);
            if (cmd.find("REQ_KEYFRAME") != std::string::npos && m_onKeyframeReq) {
                m_onKeyframeReq();
            } else if (cmd.find("GET_INFO") != std::string::npos) {
                sendToClient(buildHelloLine());
            } else if (cmd.find("PING") != std::string::npos) {
                sendToClient("PONG\n");
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_clientMutex);
            if (m_clientSock == clientSock) {
                closesocket(m_clientSock);
                m_clientSock = INVALID_SOCKET;
            }
        }
        m_btConnected = false;
    }
}
