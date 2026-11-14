#include "AirPlayServer.h"
#include <iostream>
#include <algorithm>
#include <cstdio>
#include <cstring>

AirPlayServer::AirPlayServer(int port)
    : m_port(port) {}

AirPlayServer::~AirPlayServer() {
    stop();
}

void AirPlayServer::setStreamInfo(uint32_t width, uint32_t height, uint16_t fps,
                                  VideoCodec codec, ColorProfile colorProfile,
                                  uint32_t audioSampleRate, uint16_t helloFlags,
                                  std::function<void()> onKeyframeReq) {
    {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        m_hello.width = width;
        m_hello.height = height;
        m_hello.fps = fps;
        m_hello.codec = static_cast<uint8_t>(codec);
        m_hello.colorProfile = static_cast<uint8_t>(colorProfile);
        m_hello.audioSampleRate = audioSampleRate;
        m_hello.audioChannels = 2;
        m_hello.flags = helloFlags;

        char hostname[64] = "WindowsPC";
        gethostname(hostname, sizeof(hostname) - 1);
        snprintf(m_hello.deviceName, sizeof(m_hello.deviceName), "%s v%s", hostname, WINMIRROR_VERSION);

        m_onKeyframeReq = std::move(onKeyframeReq);
    }

    // Notify already-connected clients cleanly on their single writer thread
    std::lock_guard<std::mutex> clock(m_clientsMutex);
    for (auto& c : m_clients) {
        if (c->alive) {
            {
                std::lock_guard<std::mutex> qlock(c->queueMutex);
                c->hasHelloPending = true;
            }
            c->cv.notify_one();
        }
    }
}

bool AirPlayServer::start() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[AirPlayServer] WSAStartup failed." << std::endl;
        return false;
    }

    m_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSock == INVALID_SOCKET) {
        std::cerr << "[AirPlayServer] socket() failed." << std::endl;
        return false;
    }

    int opt = 1;
    setsockopt(m_listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<u_short>(m_port));

    if (bind(m_listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "[AirPlayServer] bind() failed on port " << m_port << std::endl;
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
        return false;
    }

    if (listen(m_listenSock, 8) == SOCKET_ERROR) {
        std::cerr << "[AirPlayServer] listen() failed." << std::endl;
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
        return false;
    }

    m_running = true;
    m_acceptThread = std::thread(&AirPlayServer::acceptLoop, this);
    return true;
}

void AirPlayServer::stop() {
    if (!m_running) return;
    m_running = false;

    if (m_listenSock != INVALID_SOCKET) {
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
    }

    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto& c : m_clients) {
            c->alive = false;
            if (c->sock != INVALID_SOCKET) {
                closesocket(c->sock);
                c->sock = INVALID_SOCKET;
            }
            c->cv.notify_all();
        }
    }

    if (m_acceptThread.joinable()) {
        m_acceptThread.join();
    }

    WSACleanup();
}

void AirPlayServer::acceptLoop() {
    while (m_running) {
        sockaddr_in clientAddr = {};
        int addrLen = sizeof(clientAddr);
        SOCKET clientSock = accept(m_listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
        if (clientSock == INVALID_SOCKET) {
            if (!m_running) break;
            continue;
        }

        int nodelay = 1;
        setsockopt(clientSock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        // 128 KB send buffer prevents kernel bufferbloat over Wi-Fi/USB so congestion drops trigger promptly
        int sndbuf = 128 * 1024;
        setsockopt(clientSock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));

        int tos = 0x88; // DSCP AF41 multimedia low-latency class (best-effort)
        setsockopt(clientSock, IPPROTO_IP, IP_TOS, reinterpret_cast<const char*>(&tos), sizeof(tos));

        DWORD sndTimeoutMs = 2000;
        setsockopt(clientSock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sndTimeoutMs), sizeof(sndTimeoutMs));

        DWORD rcvTimeoutMs = 5000;
        setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcvTimeoutMs), sizeof(rcvTimeoutMs));

        auto session = std::make_shared<ClientSession>();
        session->sock = clientSock;
        session->alive = true;

        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            m_clients.push_back(session);
        }
        m_activeClients++;

        std::thread(&AirPlayServer::runClient, this, session).detach();
    }
}

bool AirPlayServer::sendAll(SOCKET sock, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    size_t sent = 0;
    while (sent < len) {
        int r = send(sock, p + sent, static_cast<int>(len - sent), 0);
        if (r <= 0) return false;
        sent += r;
    }
    return true;
}

bool AirPlayServer::sendPacket(SOCKET sock, uint16_t type, uint16_t flags, uint32_t extra1, uint32_t extra2,
                               const void* payload, uint32_t payloadSize, std::vector<uint8_t>* txScratch) {
    PacketHeader hdr = {};
    hdr.magic = PACKET_MAGIC;
    hdr.type = type;
    hdr.flags = flags;
    hdr.payloadSize = payloadSize;
    hdr.extra1 = extra1;
    hdr.extra2 = extra2;

    if (payloadSize == 0 || !payload) {
        return sendAll(sock, &hdr, sizeof(hdr));
    }

    if (txScratch) {
        // Coalesce 20-byte header + payload into one send() so Wi-Fi/LAN never splits them into two TCP segments
        txScratch->resize(sizeof(hdr) + payloadSize);
        memcpy(txScratch->data(), &hdr, sizeof(hdr));
        memcpy(txScratch->data() + sizeof(hdr), payload, payloadSize);
        return sendAll(sock, txScratch->data(), txScratch->size());
    }

    if (!sendAll(sock, &hdr, sizeof(hdr))) return false;
    return sendAll(sock, payload, payloadSize);
}

void AirPlayServer::broadcastVideo(std::shared_ptr<const EncodedFrame> frame) {
    if (!frame || frame->data.empty()) return;

    std::lock_guard<std::mutex> clock(m_clientsMutex);
    for (auto& c : m_clients) {
        if (!c->alive) continue;
        bool needKey = false;
        {
            std::lock_guard<std::mutex> qlock(c->queueMutex);
            if (c->waitingForKeyframe && !frame->isKeyframe) {
                continue;
            }
            if (frame->isKeyframe) {
                c->waitingForKeyframe = false;
                if (!c->videoQueue.empty()) {
                    c->videoQueue.clear();
                }
            }
            if (c->videoQueue.size() > 2) {
                // Tight 2-frame (~33ms) congestion cap so interactive games stay snappy
                c->videoQueue.clear();
                if (!frame->isKeyframe) {
                    c->waitingForKeyframe = true;
                    needKey = true;
                }
            }
            if (!c->waitingForKeyframe) {
                c->videoQueue.push_back(frame);
            }
        }
        c->cv.notify_one();

        if (needKey) {
            std::lock_guard<std::mutex> ilock(m_infoMutex);
            if (m_onKeyframeReq) m_onKeyframeReq();
        }
    }
}

void AirPlayServer::broadcastCursor(const CursorPayload& meta, const std::vector<uint8_t>& bgra) {
    {
        std::lock_guard<std::mutex> ilock(m_infoMutex);
        m_lastCursor = meta;
        if (meta.hasBitmap && !bgra.empty()) {
            m_lastCursorBgra = bgra;
        }
        m_hasCursor = true;
    }

    std::lock_guard<std::mutex> clock(m_clientsMutex);
    for (auto& c : m_clients) {
        if (!c->alive) continue;
        {
            std::lock_guard<std::mutex> qlock(c->queueMutex);
            bool keepOldBitmap = c->hasCursorPending && (c->pendingCursor.hasBitmap != 0) && (meta.hasBitmap == 0);
            c->pendingCursor = meta;
            if (meta.hasBitmap && !bgra.empty()) {
                c->pendingCursorBgra = bgra;
            } else if (keepOldBitmap) {
                c->pendingCursor.hasBitmap = 1;
            } else {
                c->pendingCursorBgra.clear();
            }
            c->hasCursorPending = true;
        }
        c->cv.notify_one();
    }
}

void AirPlayServer::broadcastAudio(const uint8_t* pcm, size_t size, uint32_t sampleRate, uint16_t channels) {
    if (!pcm || size == 0) return;

    auto pkt = std::make_shared<AudioPacket>();
    pkt->pcm.assign(pcm, pcm + size);
    pkt->sampleRate = sampleRate;
    pkt->channels = channels;

    std::lock_guard<std::mutex> clock(m_clientsMutex);
    for (auto& c : m_clients) {
        if (!c->alive) continue;
        {
            std::lock_guard<std::mutex> qlock(c->queueMutex);
            c->audioQueue.push_back(pkt);
            if (c->audioQueue.size() > 16) {
                c->audioQueue.pop_front();
            }
        }
        c->cv.notify_one();
    }
}

void AirPlayServer::setAudioSourceHandler(std::function<void(int)> handler) {
    std::lock_guard<std::mutex> ilock(m_infoMutex);
    m_onAudioSourceReq = std::move(handler);
}

void AirPlayServer::broadcastAudioDevices(const std::string& serializedDevices) {
    {
        std::lock_guard<std::mutex> ilock(m_infoMutex);
        m_audioDevices = serializedDevices;
    }

    std::lock_guard<std::mutex> clock(m_clientsMutex);
    for (auto& c : m_clients) {
        if (!c->alive) continue;
        {
            std::lock_guard<std::mutex> qlock(c->queueMutex);
            c->hasAudioListPending = true;
        }
        c->cv.notify_one();
    }
}

void AirPlayServer::clientRecvLoop(std::shared_ptr<ClientSession> session) {
    while (session->alive && m_running) {
        PacketHeader hdr = {};
        size_t total = 0;
        char* p = reinterpret_cast<char*>(&hdr);
        bool ok = true;
        while (total < sizeof(hdr)) {
            int r = recv(session->sock, p + total, static_cast<int>(sizeof(hdr) - total), 0);
            if (r <= 0) { ok = false; break; }
            total += r;
        }
        if (!ok || hdr.magic != PACKET_MAGIC) break;

        if (hdr.payloadSize > 0 && hdr.payloadSize <= 65536) {
            std::vector<uint8_t> discard(hdr.payloadSize);
            size_t got = 0;
            while (got < hdr.payloadSize) {
                int r = recv(session->sock, reinterpret_cast<char*>(discard.data() + got), static_cast<int>(hdr.payloadSize - got), 0);
                if (r <= 0) { ok = false; break; }
                got += r;
            }
            if (!ok) break;
        }

        if (hdr.type == PACKET_TYPE_KEYFRAME_REQ) {
            std::lock_guard<std::mutex> ilock(m_infoMutex);
            if (m_onKeyframeReq) m_onKeyframeReq();
        } else if (hdr.type == PACKET_TYPE_SET_AUDIO_SRC) {
            std::function<void(int)> fn;
            {
                std::lock_guard<std::mutex> ilock(m_infoMutex);
                fn = m_onAudioSourceReq;
            }
            if (fn) fn(static_cast<int32_t>(hdr.extra1));
        } else if (hdr.type == PACKET_TYPE_PING) {
            {
                std::lock_guard<std::mutex> qlock(session->queueMutex);
                session->hasPongPending = true;
                session->pongExtra1 = hdr.extra1;
                session->pongExtra2 = hdr.extra2;
            }
            session->cv.notify_one();
        }
    }
    session->alive = false;
    session->cv.notify_all();
}

void AirPlayServer::runClient(std::shared_ptr<ClientSession> session) {
    // 1. Send initial HELLO + cached cursor + force fresh IDR keyframe
    HelloPayload hello = {};
    CursorPayload initCursor = {};
    std::vector<uint8_t> initCursorBgra;
    std::string initAudioDevs;
    bool sendInitCursor = false;
    std::function<void()> keyReq;

    {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        hello = m_hello;
        initAudioDevs = m_audioDevices;
        keyReq = m_onKeyframeReq;
        if (m_hasCursor && !m_lastCursorBgra.empty()) {
            initCursor = m_lastCursor;
            initCursor.hasBitmap = 1;
            initCursorBgra = m_lastCursorBgra;
            sendInitCursor = true;
        }
    }

    if (!sendPacket(session->sock, PACKET_TYPE_HELLO, 0, 0, 0, &hello, sizeof(hello), &session->txBuf)) {
        session->alive = false;
    } else {
        if (!initAudioDevs.empty()) {
            sendPacket(session->sock, PACKET_TYPE_AUDIO_DEVICES, 0, 0, 0,
                       initAudioDevs.data(), static_cast<uint32_t>(initAudioDevs.size()), &session->txBuf);
        }
        if (sendInitCursor) {
            std::vector<uint8_t> payload(sizeof(CursorPayload) + initCursorBgra.size());
            memcpy(payload.data(), &initCursor, sizeof(CursorPayload));
            memcpy(payload.data() + sizeof(CursorPayload), initCursorBgra.data(), initCursorBgra.size());
            sendPacket(session->sock, PACKET_TYPE_CURSOR, 0, 0, 0, payload.data(), static_cast<uint32_t>(payload.size()), &session->txBuf);
        }
    }

    if (session->alive && keyReq) {
        keyReq();
    }

    std::thread recvThread(&AirPlayServer::clientRecvLoop, this, session);

    std::deque<std::shared_ptr<const AudioPacket>> audioBatch;
    std::vector<uint8_t> cursorBuf;

    while (session->alive && m_running) {
        std::shared_ptr<const EncodedFrame> videoFrame;
        CursorPayload curMeta = {};
        std::vector<uint8_t> curBgra;
        bool sendCur = false;
        bool sendHello = false;
        bool sendAudioList = false;
        bool sendPong = false;
        uint32_t pongE1 = 0, pongE2 = 0;

        {
            std::unique_lock<std::mutex> lock(session->queueMutex);
            session->cv.wait_for(lock, std::chrono::milliseconds(50), [&]() {
                return !session->alive || !m_running ||
                       !session->videoQueue.empty() || !session->audioQueue.empty() ||
                       session->hasCursorPending || session->hasHelloPending ||
                       session->hasAudioListPending || session->hasPongPending;
            });

            if (!session->alive || !m_running) break;

            if (session->hasHelloPending) {
                session->hasHelloPending = false;
                sendHello = true;
            }
            if (session->hasAudioListPending) {
                session->hasAudioListPending = false;
                sendAudioList = true;
            }
            if (session->hasPongPending) {
                session->hasPongPending = false;
                pongE1 = session->pongExtra1;
                pongE2 = session->pongExtra2;
                sendPong = true;
            }
            if (session->hasCursorPending) {
                curMeta = session->pendingCursor;
                curBgra = std::move(session->pendingCursorBgra);
                session->hasCursorPending = false;
                sendCur = true;
            }
            if (!session->audioQueue.empty()) {
                audioBatch.swap(session->audioQueue);
            }
            if (!session->videoQueue.empty()) {
                videoFrame = std::move(session->videoQueue.front());
                session->videoQueue.pop_front();
            }
        }

        if (sendHello) {
            HelloPayload latestHello = {};
            {
                std::lock_guard<std::mutex> ilock(m_infoMutex);
                latestHello = m_hello;
            }
            if (!sendPacket(session->sock, PACKET_TYPE_HELLO, 0, 0, 0, &latestHello, sizeof(latestHello), &session->txBuf)) {
                break;
            }
        }

        if (sendAudioList) {
            std::string devsCopy;
            {
                std::lock_guard<std::mutex> ilock(m_infoMutex);
                devsCopy = m_audioDevices;
            }
            if (!devsCopy.empty()) {
                if (!sendPacket(session->sock, PACKET_TYPE_AUDIO_DEVICES, 0, 0, 0,
                                devsCopy.data(), static_cast<uint32_t>(devsCopy.size()), &session->txBuf)) {
                    break;
                }
            }
        }

        if (sendPong) {
            if (!sendPacket(session->sock, PACKET_TYPE_PONG, 0, pongE1, pongE2, nullptr, 0)) {
                break;
            }
        }

        // Priority 1: Cursor (zero-latency 120Hz mouse feel)
        if (sendCur) {
            cursorBuf.resize(sizeof(CursorPayload) + curBgra.size());
            memcpy(cursorBuf.data(), &curMeta, sizeof(CursorPayload));
            if (!curBgra.empty()) {
                memcpy(cursorBuf.data() + sizeof(CursorPayload), curBgra.data(), curBgra.size());
            }
            if (!sendPacket(session->sock, PACKET_TYPE_CURSOR, 0, 0, 0,
                            cursorBuf.data(), static_cast<uint32_t>(cursorBuf.size()), &session->txBuf)) {
                break;
            }
        }

        // Priority 2: Audio PCM (per-client queue, zero delay)
        if (!audioBatch.empty()) {
            for (const auto& pkt : audioBatch) {
                if (!sendPacket(session->sock, PACKET_TYPE_AUDIO, 0, pkt->sampleRate,
                                static_cast<uint32_t>(pkt->channels) | (16u << 16),
                                pkt->pcm.data(), static_cast<uint32_t>(pkt->pcm.size()), &session->txBuf)) {
                    session->alive = false;
                    break;
                }
            }
            audioBatch.clear();
            if (!session->alive) break;
        }

        // Priority 3: Hardware Encoded Video Frame
        if (videoFrame) {
            uint16_t flags = (videoFrame->isKeyframe ? 1 : 0) |
                             ((videoFrame->colorProfile == COLOR_HDR10_PQ) ? 2 : 0);
            uint32_t extra1 = (static_cast<uint32_t>(videoFrame->codec) << 24) |
                              (static_cast<uint32_t>(videoFrame->colorProfile) << 16) |
                              (videoFrame->width & 0xFFFF);
            if (!sendPacket(session->sock, PACKET_TYPE_VIDEO, flags, extra1, videoFrame->ptsMs,
                            videoFrame->data.data(), static_cast<uint32_t>(videoFrame->data.size()), &session->txBuf)) {
                break;
            }
        }
    }

    session->alive = false;
    if (session->sock != INVALID_SOCKET) {
        closesocket(session->sock);
        session->sock = INVALID_SOCKET;
    }
    if (recvThread.joinable()) {
        recvThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clients.erase(std::remove(m_clients.begin(), m_clients.end(), session), m_clients.end());
    }
    m_activeClients--;
}
