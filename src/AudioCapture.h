#pragma once

#include "Common.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>

typedef std::function<void(const uint8_t* pcmData, size_t size, uint32_t sampleRate, uint16_t channels)> AudioDataCallback;
typedef std::function<void(const std::string& deviceListSerialized)> AudioDevicesCallback;

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    bool init(AudioDataCallback callback, AudioDevicesCallback devCallback = nullptr);
    void start();
    void stop();

    void setSourceDeviceIndex(int index);
    std::string getDeviceListSerialized();

    bool isRunning() const { return m_running.load(); }
    uint32_t getSampleRate() const { return m_sampleRate.load(); }

private:
    void captureLoop();
    bool initEndpoint();
    bool tryActivateDevice(IMMDevice* dev);
    void refreshDeviceList();
    void releaseEndpoint();
    void cleanup();

    AudioDataCallback m_callback;
    AudioDevicesCallback m_devCallback;
    std::atomic<bool> m_running{false};
    std::atomic<uint32_t> m_sampleRate{48000};
    std::atomic<int> m_requestedIndex{-1};
    std::atomic<bool> m_forceReinit{false};
    std::mutex m_devListMutex;
    std::string m_cachedDeviceList;
    std::wstring m_targetDeviceId;
    std::thread m_thread;

    IMMDeviceEnumerator* m_enumerator = nullptr;
    IMMDevice* m_device = nullptr;
    IAudioClient* m_audioClient = nullptr;
    IAudioCaptureClient* m_captureClient = nullptr;
    WAVEFORMATEX* m_pwfx = nullptr;
    std::wstring m_deviceId;
};
