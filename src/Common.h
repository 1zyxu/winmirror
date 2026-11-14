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
#include <deque>
#include <mutex>
#include <atomic>
#include <chrono>

#define WINMIRROR_VERSION "1.0.5"
#define WINMIRROR_REPO "1zyxu/winmirror"
#define DEFAULT_PORT 8888
#define BEACON_PORT 8889

#define PACKET_MAGIC 0x574D4952 // 'WMIR' in little-endian

enum PacketType : uint16_t {
    PACKET_TYPE_VIDEO = 0x01,
    PACKET_TYPE_AUDIO = 0x02,
    PACKET_TYPE_PING  = 0x03
};

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;         // PACKET_MAGIC ('WMIR')
    uint16_t type;          // PacketType (1=Video, 2=Audio)
    uint16_t flags;         // Reserved (0)
    uint32_t payloadSize;   // Total bytes following header
    uint32_t extra1;        // Video: width, Audio: sampleRate (48000)
    uint32_t extra2;        // Video: height, Audio: channels (2) | (bitsPerSample << 16)
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

class AudioBuffer {
public:
    void push(const uint8_t* pcm, size_t size) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_chunks.emplace_back(pcm, pcm + size);
        if (m_chunks.size() > 50) {
            m_chunks.pop_front();
        }
    }

    bool popAll(std::vector<std::vector<uint8_t>>& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_chunks.empty()) return false;
        out.reserve(m_chunks.size());
        for (auto& chunk : m_chunks) {
            out.push_back(std::move(chunk));
        }
        m_chunks.clear();
        return true;
    }

private:
    std::mutex m_mutex;
    std::deque<std::vector<uint8_t>> m_chunks;
};
