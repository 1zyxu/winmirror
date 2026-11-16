#pragma once

#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <chrono>
#include <memory>

#define WINMIRROR_VERSION "2.5.5"
#define WINMIRROR_PUBLISHER "karnajitop"
#define AIRPLAY_PORT 7001
#define BEACON_PORT 8889

// Bluetooth RFCOMM SDP Service UUID: {8F4A2C10-7B3E-4E91-9A02-574D49520001}
// Stored as 128-bit GUID bytes
static constexpr GUID WINMIRROR_BT_SERVICE_UUID = {
    0x8F4A2C10, 0x7B3E, 0x4E91, { 0x9A, 0x02, 0x57, 0x4D, 0x49, 0x52, 0x00, 0x01 }
};

#define PACKET_MAGIC 0x574D4952 // 'WMIR'

enum VideoCodec : uint8_t {
    CODEC_H264 = 1,
    CODEC_HEVC = 2
};

enum ColorProfile : uint8_t {
    COLOR_SDR_BT709  = 1,
    COLOR_HDR10_PQ   = 2
};

enum PacketType : uint16_t {
    PACKET_TYPE_HELLO         = 0x01,
    PACKET_TYPE_VIDEO         = 0x02,
    PACKET_TYPE_AUDIO         = 0x03,
    PACKET_TYPE_CURSOR        = 0x04,
    PACKET_TYPE_PING          = 0x05,
    PACKET_TYPE_PONG          = 0x06,
    PACKET_TYPE_KEYFRAME_REQ  = 0x07,
    PACKET_TYPE_AUDIO_DEVICES = 0x08,
    PACKET_TYPE_SET_AUDIO_SRC = 0x09,
    PACKET_TYPE_GOODBYE       = 0x0A
};

enum HelloFlags : uint16_t {
    HELLO_FLAG_BT            = 1 << 0,
    HELLO_FLAG_NVIDIA        = 1 << 1,
    HELLO_FLAG_ZEROCOPY      = 1 << 2,
    HELLO_FLAG_HW_GPU        = 1 << 3,
    HELLO_FLAG_UNLIMITED_FPS = 1 << 4
};

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;       // PACKET_MAGIC
    uint16_t type;        // PacketType
    uint16_t flags;       // Video: bit0 = IDR keyframe; bit1 = HDR10 PQ
    uint32_t payloadSize; // Payload byte count
    uint32_t extra1;      // Video: (codec<<24)|(colorProfile<<16)|width_low16 ; Audio: sampleRate
    uint32_t extra2;      // Video: ptsMs ; Audio: channels | (bitsPerSample << 16)
};

struct HelloPayload {
    uint32_t width;
    uint32_t height;
    uint16_t fps;
    uint8_t codec;          // VideoCodec
    uint8_t colorProfile;   // ColorProfile
    uint32_t audioSampleRate;
    uint16_t audioChannels;
    uint16_t flags;         // bit0 = bluetoothPaired
    char deviceName[64];
};

struct CursorPayload {
    int32_t x;
    int32_t y;
    uint32_t screenW;
    uint32_t screenH;
    int16_t hotspotX;
    int16_t hotspotY;
    uint16_t width;
    uint16_t height;
    uint8_t visible;
    uint8_t hasBitmap;      // 1 = BGRA bitmap follows immediately (width * height * 4 bytes)
    uint16_t reserved;
};
#pragma pack(pop)

struct EncodedFrame {
    std::vector<uint8_t> data;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t ptsMs = 0;
    uint8_t codec = CODEC_H264;
    uint8_t colorProfile = COLOR_SDR_BT709;
    bool isKeyframe = false;
    uint64_t seq = 0;
};

struct AudioPacket {
    std::vector<uint8_t> pcm;
    uint32_t sampleRate = 48000;
    uint16_t channels = 2;
};

struct AirPlayTarget {
    std::string name;
    std::string ip;
    int port = 7002;
    bool isLaptop = true;
    bool isDirectObs = false;
    bool isConnected = false;
};
