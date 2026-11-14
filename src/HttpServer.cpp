#include "HttpServer.h"
#include <iostream>
#include <sstream>
#include <cstring>

static const char* INDEX_HTML = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>WinMirror &mdash; Ultra-Low Latency PC Mirror</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: #090a0f;
    color: #e2e8f0;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    overflow: hidden;
    height: 100vh;
    display: flex;
    flex-direction: column;
  }
  #stage {
    flex: 1;
    display: flex;
    align-items: center;
    justify-content: center;
    position: relative;
    background: radial-gradient(circle at center, #111422 0%, #08090d 100%);
  }
  #stream {
    max-width: 100%;
    max-height: 100%;
    object-fit: contain;
    user-select: none;
    box-shadow: 0 10px 40px rgba(0, 0, 0, 0.7);
  }
  #overlay {
    position: absolute;
    top: 16px;
    left: 16px;
    display: flex;
    gap: 8px;
    z-index: 10;
    transition: opacity 0.3s;
  }
  .badge {
    background: rgba(15, 23, 42, 0.75);
    backdrop-filter: blur(8px);
    border: 1px solid rgba(255, 255, 255, 0.1);
    padding: 6px 12px;
    border-radius: 999px;
    font-size: 12px;
    font-weight: 500;
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    background: #10b981;
    box-shadow: 0 0 10px #10b981;
    animation: pulse 2s infinite;
  }
  @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.4; } }
  #controls {
    position: absolute;
    bottom: 20px;
    right: 20px;
    display: flex;
    gap: 10px;
    z-index: 10;
  }
  .btn {
    background: rgba(30, 41, 59, 0.85);
    color: #fff;
    border: 1px solid rgba(255, 255, 255, 0.15);
    padding: 8px 16px;
    border-radius: 8px;
    cursor: pointer;
    font-size: 13px;
    font-weight: 500;
    transition: all 0.2s;
  }
  .btn:hover {
    background: rgba(51, 65, 85, 0.95);
    border-color: rgba(255, 255, 255, 0.3);
  }
</style>
</head>
<body>
<div id="stage">
  <div id="overlay">
    <div class="badge"><span class="dot"></span> <span id="status">Connected (Direct USB/LAN)</span></div>
    <div class="badge" id="fpsBadge">60 FPS</div>
    <div class="badge" id="resBadge">-- x --</div>
  </div>
  <img id="stream" src="/stream" alt="PC Display Stream" />
  <div id="controls">
    <button class="btn" id="fsBtn">Fullscreen (F)</button>
    <button class="btn" id="fitBtn">Fit / Stretch</button>
  </div>
</div>

<script>
  const img = document.getElementById('stream');
  const fpsBadge = document.getElementById('fpsBadge');
  const resBadge = document.getElementById('resBadge');
  const fsBtn = document.getElementById('fsBtn');
  const fitBtn = document.getElementById('fitBtn');

  let frames = 0;
  let lastTime = performance.now();

  function countFps() {
    frames++;
    const now = performance.now();
    if (now - lastTime >= 1000) {
      fpsBadge.textContent = Math.round((frames * 1000) / (now - lastTime)) + ' FPS';
      frames = 0;
      lastTime = now;
      if (img.naturalWidth) {
        resBadge.textContent = img.naturalWidth + ' x ' + img.naturalHeight;
      }
    }
    requestAnimationFrame(countFps);
  }
  requestAnimationFrame(countFps);

  fsBtn.addEventListener('click', () => {
    if (!document.fullscreenElement) {
      document.documentElement.requestFullscreen();
      fsBtn.textContent = 'Exit Fullscreen';
    } else {
      document.exitFullscreen();
      fsBtn.textContent = 'Fullscreen (F)';
    }
  });

  window.addEventListener('keydown', (e) => {
    if (e.key === 'f' || e.key === 'F') {
      fsBtn.click();
    }
  });

  let fitMode = 'contain';
  fitBtn.addEventListener('click', () => {
    fitMode = (fitMode === 'contain') ? 'cover' : 'contain';
    img.style.objectFit = fitMode;
    fitBtn.textContent = (fitMode === 'contain') ? 'Fit (Contain)' : 'Fill (Stretch)';
  });
</script>
</body>
</html>)rawhtml";

HttpServer::HttpServer(FrameBuffer& frameBuffer, int port)
    : m_frameBuffer(frameBuffer), m_port(port) {}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[HttpServer] WSAStartup failed." << std::endl;
        return false;
    }

    m_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSock == INVALID_SOCKET) {
        std::cerr << "[HttpServer] Failed to create socket." << std::endl;
        return false;
    }

    int reuse = 1;
    setsockopt(m_listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<u_short>(m_port));

    if (bind(m_listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "[HttpServer] Failed to bind to port " << m_port << "." << std::endl;
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
        return false;
    }

    if (listen(m_listenSock, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "[HttpServer] Listen failed." << std::endl;
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
        return false;
    }

    m_running = true;
    m_acceptThread = std::thread(&HttpServer::acceptLoop, this);
    return true;
}

void HttpServer::stop() {
    if (!m_running) return;
    m_running = false;

    if (m_listenSock != INVALID_SOCKET) {
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
    }

    if (m_acceptThread.joinable()) {
        m_acceptThread.join();
    }

    WSACleanup();
}

void HttpServer::acceptLoop() {
    while (m_running) {
        sockaddr_in clientAddr = {};
        int addrLen = sizeof(clientAddr);
        SOCKET clientSock = accept(m_listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);

        if (clientSock == INVALID_SOCKET) {
            if (!m_running) break;
            continue;
        }

        // Optimize socket for ultra-low latency
        int nodelay = 1;
        setsockopt(clientSock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        int sndbuf = 512 * 1024;
        setsockopt(clientSock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));

        // Detach client handling thread
        std::thread(&HttpServer::handleClient, this, clientSock).detach();
    }
}

void HttpServer::handleClient(SOCKET clientSock) {
    char buffer[2048] = {};
    int bytes = recv(clientSock, buffer, sizeof(buffer) - 1, 0);
    if (bytes <= 0) {
        closesocket(clientSock);
        return;
    }

    std::string request(buffer, bytes);
    std::string path = "/";

    size_t firstSpace = request.find(' ');
    if (firstSpace != std::string::npos) {
        size_t secondSpace = request.find(' ', firstSpace + 1);
        if (secondSpace != std::string::npos) {
            path = request.substr(firstSpace + 1, secondSpace - firstSpace - 1);
        }
    }

    if (path == "/" || path == "/index.html") {
        handleIndex(clientSock);
        closesocket(clientSock);
    } else if (path == "/stream") {
        m_activeClients++;
        handleStream(clientSock);
        m_activeClients--;
        closesocket(clientSock);
    } else if (path == "/snapshot") {
        handleSnapshot(clientSock);
        closesocket(clientSock);
    } else if (path == "/api/status") {
        handleStatus(clientSock);
        closesocket(clientSock);
    } else {
        const char* notFound = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        send(clientSock, notFound, static_cast<int>(strlen(notFound)), 0);
        closesocket(clientSock);
    }
}

void HttpServer::handleIndex(SOCKET clientSock) {
    size_t len = strlen(INDEX_HTML);
    std::ostringstream ss;
    ss << "HTTP/1.1 200 OK\r\n"
       << "Content-Type: text/html; charset=utf-8\r\n"
       << "Content-Length: " << len << "\r\n"
       << "Connection: close\r\n\r\n"
       << INDEX_HTML;
    std::string resp = ss.str();
    send(clientSock, resp.c_str(), static_cast<int>(resp.size()), 0);
}

void HttpServer::handleSnapshot(SOCKET clientSock) {
    FrameData frame;
    if (!m_frameBuffer.getLatest(frame)) {
        const char* empty = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";
        send(clientSock, empty, static_cast<int>(strlen(empty)), 0);
        return;
    }

    std::ostringstream ss;
    ss << "HTTP/1.1 200 OK\r\n"
       << "Content-Type: image/jpeg\r\n"
       << "Content-Length: " << frame.jpegData.size() << "\r\n"
       << "Access-Control-Allow-Origin: *\r\n"
       << "Cache-Control: no-cache, no-store\r\n"
       << "Connection: close\r\n\r\n";
    std::string header = ss.str();
    send(clientSock, header.c_str(), static_cast<int>(header.size()), 0);
    send(clientSock, reinterpret_cast<const char*>(frame.jpegData.data()), static_cast<int>(frame.jpegData.size()), 0);
}

void HttpServer::handleStatus(SOCKET clientSock) {
    FrameData frame;
    m_frameBuffer.getLatest(frame);

    std::ostringstream json;
    json << "{\"name\":\"WinMirror\",\"version\":\"" << WINMIRROR_VERSION
         << "\",\"clients\":" << m_activeClients.load()
         << ",\"width\":" << frame.width
         << ",\"height\":" << frame.height
         << ",\"timestamp\":" << frame.timestamp << "}";

    std::string body = json.str();
    std::ostringstream ss;
    ss << "HTTP/1.1 200 OK\r\n"
       << "Content-Type: application/json\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Access-Control-Allow-Origin: *\r\n"
       << "Connection: close\r\n\r\n"
       << body;
    std::string resp = ss.str();
    send(clientSock, resp.c_str(), static_cast<int>(resp.size()), 0);
}

void HttpServer::handleStream(SOCKET clientSock) {
    const char* initHeader =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=--frame\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Connection: close\r\n\r\n";

    if (send(clientSock, initHeader, static_cast<int>(strlen(initHeader)), 0) <= 0) {
        return;
    }

    uint64_t lastFrame = 0;
    while (m_running) {
        FrameData frame;
        if (!m_frameBuffer.getLatest(frame, lastFrame)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        lastFrame = frame.frameNumber;

        char frameHeader[256];
        int hlen = snprintf(frameHeader, sizeof(frameHeader),
            "--frame\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %zu\r\n"
            "X-Timestamp: %llu\r\n\r\n",
            frame.jpegData.size(),
            static_cast<unsigned long long>(frame.timestamp));

        if (send(clientSock, frameHeader, hlen, 0) <= 0) break;
        if (send(clientSock, reinterpret_cast<const char*>(frame.jpegData.data()), static_cast<int>(frame.jpegData.size()), 0) <= 0) break;
        if (send(clientSock, "\r\n", 2, 0) <= 0) break;
    }
}
