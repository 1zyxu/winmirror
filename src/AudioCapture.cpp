#include "AudioCapture.h"
#include <iostream>
#include <vector>
#include <algorithm>

AudioCapture::AudioCapture() = default;

AudioCapture::~AudioCapture() {
    stop();
    cleanup();
}

void AudioCapture::cleanup() {
    if (m_pwfx) {
        CoTaskMemFree(m_pwfx);
        m_pwfx = nullptr;
    }
    if (m_captureClient) {
        m_captureClient->Release();
        m_captureClient = nullptr;
    }
    if (m_audioClient) {
        m_audioClient->Release();
        m_audioClient = nullptr;
    }
    if (m_device) {
        m_device->Release();
        m_device = nullptr;
    }
    if (m_enumerator) {
        m_enumerator->Release();
        m_enumerator = nullptr;
    }
}

bool AudioCapture::init(AudioDataCallback callback) {
    cleanup();
    m_callback = callback;

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&m_enumerator)
    );
    if (FAILED(hr)) return false;

    hr = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
    if (FAILED(hr)) { cleanup(); return false; }

    hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&m_audioClient));
    if (FAILED(hr)) { cleanup(); return false; }

    hr = m_audioClient->GetMixFormat(&m_pwfx);
    if (FAILED(hr)) { cleanup(); return false; }

    // Initialize in low-latency loopback mode
    hr = m_audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        2000000, // 200ms buffer for low latency
        0,
        m_pwfx,
        nullptr
    );
    if (FAILED(hr)) { cleanup(); return false; }

    hr = m_audioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&m_captureClient));
    if (FAILED(hr)) { cleanup(); return false; }

    return true;
}

void AudioCapture::start() {
    if (m_running || !m_audioClient) return;

    m_audioClient->Start();
    m_running = true;
    m_thread = std::thread(&AudioCapture::captureLoop, this);
    std::cout << "[AudioCapture] WASAPI Loopback started (" << m_pwfx->nSamplesPerSec
              << " Hz Stereo)" << std::endl;
}

void AudioCapture::stop() {
    if (!m_running) return;
    m_running = false;

    if (m_audioClient) {
        m_audioClient->Stop();
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void AudioCapture::captureLoop() {
    std::vector<int16_t> pcmOut;
    pcmOut.reserve(48000 * 2);

    const bool isFloat = (m_pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
        (m_pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_pwfx)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

    const int channels = m_pwfx->nChannels;
    const uint32_t sampleRate = m_pwfx->nSamplesPerSec;

    while (m_running) {
        UINT32 packetLength = 0;
        HRESULT hr = m_captureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) break;

        if (packetLength == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        while (packetLength > 0) {
            BYTE* pData = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;

            hr = m_captureClient->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            if (numFrames > 0 && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                pcmOut.resize(numFrames * 2);

                if (isFloat) {
                    const float* fData = reinterpret_cast<const float*>(pData);
                    for (UINT32 i = 0; i < numFrames; ++i) {
                        float left = fData[i * channels + 0];
                        float right = (channels > 1) ? fData[i * channels + 1] : left;

                        left = std::max(-1.0f, std::min(1.0f, left));
                        right = std::max(-1.0f, std::min(1.0f, right));

                        pcmOut[i * 2 + 0] = static_cast<int16_t>(left * 32767.0f);
                        pcmOut[i * 2 + 1] = static_cast<int16_t>(right * 32767.0f);
                    }
                } else {
                    const int16_t* sData = reinterpret_cast<const int16_t*>(pData);
                    for (UINT32 i = 0; i < numFrames; ++i) {
                        pcmOut[i * 2 + 0] = sData[i * channels + 0];
                        pcmOut[i * 2 + 1] = (channels > 1) ? sData[i * channels + 1] : sData[i * channels + 0];
                    }
                }

                if (m_callback) {
                    m_callback(reinterpret_cast<const uint8_t*>(pcmOut.data()), pcmOut.size() * sizeof(int16_t), sampleRate, 2);
                }
            }

            m_captureClient->ReleaseBuffer(numFrames);
            hr = m_captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;
        }
    }
}
