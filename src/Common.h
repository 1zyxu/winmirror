#pragma once

#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>

#define WINMIRROR_VERSION "1.0.3"
#define WINMIRROR_REPO "1zyxu/winmirror"
#define DEFAULT_PORT 8888
#define BEACON_PORT 8889

#define RAW_FRAME_MAGIC 0x52494D57 // 'WMIR' in little-endian

#pragma pack(push, 1)
struct RawFrameHeader {
    uint32_t magic;       // RAW_FRAME_MAGIC
    uint32_t width;       // Physical Width (e.g. 1920, 3840)
    uint32_t height;      // Physical Height (e.g. 1080, 2160)
    uint32_t payloadSize; // JPEG payload bytes following header
};
#pragma pack(pop)

struct FrameData {
    std::vector<uint8_t> jpegData;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t timestamp = 0;
    uint64_t frameNumber = 0;
};

class FrameBuffer {
public:
    void push(std::vector<uint8_t>&& jpeg, uint32_t width, uint32_t height) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_frame.jpegData = std::move(jpeg);
        m_frame.width = width;
        m_frame.height = height;
        m_frame.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        m_frame.frameNumber++;
        m_hasFrame = true;
    }

    bool getLatest(FrameData& out, uint64_t lastSeenFrame = 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_hasFrame || m_frame.frameNumber <= lastSeenFrame) {
            return false;
        }
        out = m_frame;
        return true;
    }

    uint64_t getCurrentFrameNumber() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_frame.frameNumber;
    }

private:
    std::mutex m_mutex;
    FrameData m_frame;
    bool m_hasFrame = false;
};
