#pragma once

#include "Common.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <thread>
#include <atomic>
#include <functional>

typedef std::function<void(const uint8_t* pcmData, size_t size, uint32_t sampleRate, uint16_t channels)> AudioDataCallback;

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    bool init(AudioDataCallback callback);
    void start();
    void stop();

    bool isRunning() const { return m_running.load(); }
    uint32_t getSampleRate() const { return m_pwfx ? m_pwfx->nSamplesPerSec : 48000; }

private:
    void captureLoop();
    void cleanup();

    AudioDataCallback m_callback;
    std::atomic<bool> m_running{false};
    std::thread m_thread;

    IMMDeviceEnumerator* m_enumerator = nullptr;
    IMMDevice* m_device = nullptr;
    IAudioClient* m_audioClient = nullptr;
    IAudioCaptureClient* m_captureClient = nullptr;
    WAVEFORMATEX* m_pwfx = nullptr;
};
