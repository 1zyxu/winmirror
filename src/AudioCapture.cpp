#include "AudioCapture.h"
#include <iostream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <propsys.h>

static const PROPERTYKEY WINMIRROR_PKEY_FriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14
};

static std::string wideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};
    std::string out(need - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], need, nullptr, nullptr);
    for (char& ch : out) {
        if (ch == '|' || ch == '\n' || ch == '\r') ch = ' ';
    }
    return out;
}

AudioCapture::AudioCapture() = default;

AudioCapture::~AudioCapture() {
    stop();
    cleanup();
}

void AudioCapture::releaseEndpoint() {
    if (m_audioClient) {
        m_audioClient->Stop();
    }
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
    m_deviceId.clear();
}

void AudioCapture::cleanup() {
    releaseEndpoint();
    if (m_enumerator) {
        m_enumerator->Release();
        m_enumerator = nullptr;
    }
}

void AudioCapture::refreshDeviceList() {
    if (!m_enumerator) return;

    int reqIdx = m_requestedIndex.load();
    std::wstring targetId;
    {
        std::lock_guard<std::mutex> lock(m_devListMutex);
        targetId = m_targetDeviceId;
    }

    std::ostringstream oss;
    auto appendFlow = [&](EDataFlow flow, int baseIdx, const char* prefix, int autoIdx, const char* autoLabel) {
        oss << autoIdx << "|" << (reqIdx == autoIdx ? 1 : 0) << "|" << autoLabel << "\n";
        IMMDeviceCollection* coll = nullptr;
        if (SUCCEEDED(m_enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll)) && coll) {
            UINT count = 0;
            coll->GetCount(&count);
            for (UINT i = 0; i < count && i < 900; ++i) {
                IMMDevice* dev = nullptr;
                if (SUCCEEDED(coll->Item(i, &dev)) && dev) {
                    int wireIdx = baseIdx + static_cast<int>(i);
                    std::string name = (flow == eCapture ? "Input Device " : "Output Device ") + std::to_string(i + 1);
                    bool selected = (reqIdx == wireIdx);

                    LPWSTR devId = nullptr;
                    if (SUCCEEDED(dev->GetId(&devId)) && devId) {
                        if (reqIdx >= 0 && !targetId.empty()) {
                            selected = (targetId == devId);
                        }
                        CoTaskMemFree(devId);
                    }

                    IPropertyStore* props = nullptr;
                    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
                        PROPVARIANT pv;
                        PropVariantInit(&pv);
                        if (SUCCEEDED(props->GetValue(WINMIRROR_PKEY_FriendlyName, &pv)) &&
                            pv.vt == VT_LPWSTR && pv.pwszVal) {
                            std::string conv = wideToUtf8(pv.pwszVal);
                            if (!conv.empty()) name = std::move(conv);
                        }
                        PropVariantClear(&pv);
                        props->Release();
                    }
                    oss << wireIdx << "|" << (selected ? 1 : 0) << "|" << prefix << name << "\n";
                    dev->Release();
                }
            }
            coll->Release();
        }
    };

    appendFlow(eRender, 0, "OUT:", -1, "OUT:Default PC Output (Auto)");
    appendFlow(eCapture, 1000, "IN:", -2, "IN:Default PC Input / Mic (Auto)");

    std::string listStr = oss.str();
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_devListMutex);
        if (listStr != m_cachedDeviceList) {
            m_cachedDeviceList = listStr;
            changed = true;
        }
    }
    if (changed && m_devCallback) {
        m_devCallback(listStr);
    }
}

void AudioCapture::setSourceDeviceIndex(int index) {
    {
        std::lock_guard<std::mutex> lock(m_devListMutex);
        m_targetDeviceId.clear();
    }
    m_requestedIndex.store(index < -2 ? -1 : index);
    m_forceReinit.store(true);
}

std::string AudioCapture::getDeviceListSerialized() {
    std::lock_guard<std::mutex> lock(m_devListMutex);
    return m_cachedDeviceList;
}

std::string AudioCapture::getActiveDeviceName() {
    std::lock_guard<std::mutex> lock(m_devListMutex);
    return m_activeDeviceName;
}

bool AudioCapture::tryActivateDevice(IMMDevice* dev, bool isInput) {
    if (!dev) return false;

    IAudioClient* client = nullptr;
    IAudioCaptureClient* cap = nullptr;
    WAVEFORMATEX* pwfx = nullptr;

    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    if (FAILED(hr) || !client) return false;

    hr = client->GetMixFormat(&pwfx);
    if (FAILED(hr) || !pwfx) {
        client->Release();
        return false;
    }

    // eRender requires loopback flag; eCapture must pass 0 (direct mic/line/virtual input)
    DWORD streamFlags = isInput ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK;
    hr = client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        streamFlags,
        1000000, // 100ms ring buffer for minimal WASAPI overhead
        0,
        pwfx,
        nullptr
    );
    if (FAILED(hr)) {
        CoTaskMemFree(pwfx);
        client->Release();
        return false;
    }

    hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cap));
    if (FAILED(hr) || !cap) {
        CoTaskMemFree(pwfx);
        client->Release();
        return false;
    }

    m_device = dev;
    m_device->AddRef();
    m_audioClient = client;
    m_captureClient = cap;
    m_pwfx = pwfx;

    m_deviceId.clear();
    LPWSTR devId = nullptr;
    if (SUCCEEDED(m_device->GetId(&devId)) && devId) {
        m_deviceId = devId;
        CoTaskMemFree(devId);
    }

    std::string friendly = isInput ? "Input Device" : "Default Output";
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(m_device->OpenPropertyStore(STGM_READ, &props)) && props) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(WINMIRROR_PKEY_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
            std::string conv = wideToUtf8(pv.pwszVal);
            if (!conv.empty()) friendly = std::move(conv);
        }
        PropVariantClear(&pv);
        props->Release();
    }
    {
        std::lock_guard<std::mutex> lock(m_devListMutex);
        m_activeDeviceName = (isInput ? "IN: " : "OUT: ") + friendly;
    }

    m_sampleRate.store(m_pwfx->nSamplesPerSec);
    return true;
}

bool AudioCapture::initEndpoint() {
    releaseEndpoint();
    if (!m_enumerator) return false;

    int reqIdx = m_requestedIndex.load();
    std::wstring targetId;
    {
        std::lock_guard<std::mutex> lock(m_devListMutex);
        targetId = m_targetDeviceId;
    }

    auto tryFlow = [&](EDataFlow flow, int baseIdx, int pinnedRelIdx) -> bool {
        bool isInput = (flow == eCapture);
        IMMDeviceCollection* coll = nullptr;
        UINT activeCount = 0;
        if (SUCCEEDED(m_enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll)) && coll) {
            coll->GetCount(&activeCount);
        }

        // 1. Pinned device by stable ID or relative index
        if (pinnedRelIdx >= 0 && coll) {
            IMMDevice* chosen = nullptr;
            if (!targetId.empty()) {
                for (UINT i = 0; i < activeCount; ++i) {
                    IMMDevice* cand = nullptr;
                    if (SUCCEEDED(coll->Item(i, &cand)) && cand) {
                        LPWSTR cid = nullptr;
                        bool match = (SUCCEEDED(cand->GetId(&cid)) && cid && targetId == cid);
                        if (cid) CoTaskMemFree(cid);
                        if (match) {
                            chosen = cand;
                            m_requestedIndex.store(baseIdx + static_cast<int>(i));
                            break;
                        }
                        cand->Release();
                    }
                }
            } else if (static_cast<UINT>(pinnedRelIdx) < activeCount) {
                coll->Item(static_cast<UINT>(pinnedRelIdx), &chosen);
            }

            if (chosen) {
                bool ok = tryActivateDevice(chosen, isInput);
                chosen->Release();
                if (ok) {
                    std::lock_guard<std::mutex> lock(m_devListMutex);
                    m_targetDeviceId = m_deviceId;
                    coll->Release();
                    return true;
                }
            }

            // Pinned device unplugged or busy: reset to that flow's auto mode
            m_requestedIndex.store(isInput ? -2 : -1);
            std::lock_guard<std::mutex> lock(m_devListMutex);
            m_targetDeviceId.clear();
        }

        // 2. Default endpoint for this flow
        for (ERole role : { eConsole, eMultimedia, eCommunications }) {
            IMMDevice* defDev = nullptr;
            if (SUCCEEDED(m_enumerator->GetDefaultAudioEndpoint(flow, role, &defDev)) && defDev) {
                bool ok = tryActivateDevice(defDev, isInput);
                defDev->Release();
                if (ok) {
                    if (coll) coll->Release();
                    return true;
                }
            }
        }

        // 3. Fallback: any active endpoint for this flow
        if (coll) {
            for (UINT i = 0; i < activeCount; ++i) {
                IMMDevice* cand = nullptr;
                if (SUCCEEDED(coll->Item(i, &cand)) && cand) {
                    bool ok = tryActivateDevice(cand, isInput);
                    cand->Release();
                    if (ok) {
                        coll->Release();
                        return true;
                    }
                }
            }
            coll->Release();
        }
        return false;
    };

    bool ok = false;
    if (reqIdx == -2 || reqIdx >= 1000) {
        ok = tryFlow(eCapture, 1000, reqIdx >= 1000 ? (reqIdx - 1000) : -1);
        if (!ok) {
            m_requestedIndex.store(-1);
            ok = tryFlow(eRender, 0, -1);
        }
    } else {
        ok = tryFlow(eRender, 0, reqIdx);
    }

    refreshDeviceList();
    return ok;
}

bool AudioCapture::init(AudioDataCallback callback, AudioDevicesCallback devCallback) {
    cleanup();
    m_callback = callback;
    m_devCallback = devCallback;

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&m_enumerator)
    );
    if (FAILED(hr) || !m_enumerator) return false;

    // Best-effort initial endpoint bind; captureLoop continuously recovers when unplugged/replugged
    initEndpoint();
    return true;
}

void AudioCapture::start() {
    if (m_running || !m_enumerator) return;

    if (m_audioClient) {
        m_audioClient->Start();
    }
    m_running = true;
    m_thread = std::thread(&AudioCapture::captureLoop, this);
    if (m_pwfx) {
        std::cout << "[AudioCapture] WASAPI Loopback started (" << m_pwfx->nSamplesPerSec
                  << " Hz Stereo)" << std::endl;
    } else {
        std::cout << "[AudioCapture] Waiting for active PC playback device (auto-attach enabled)" << std::endl;
    }
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
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::vector<int16_t> pcmOut;
    pcmOut.reserve(48000 * 2);
    int silentTailLeft = 0;
    auto lastEndpointPoll = std::chrono::steady_clock::now();
    auto lastListPoll = lastEndpointPoll;

    while (m_running) {
        auto now = std::chrono::steady_clock::now();

        if (m_forceReinit.exchange(false)) {
            if (initEndpoint() && m_audioClient && SUCCEEDED(m_audioClient->Start())) {
                silentTailLeft = 0;
            }
        }

        // Refresh full device inventory every 2s (heavy IPropertyStore), check default unplug at 400ms
        if (now - lastListPoll >= std::chrono::milliseconds(2000)) {
            lastListPoll = now;
            refreshDeviceList();
        }
        if (now - lastEndpointPoll >= std::chrono::milliseconds(400)) {
            lastEndpointPoll = now;

            int reqIdxNow = m_requestedIndex.load();
            if (reqIdxNow < 0 && m_enumerator && m_captureClient) {
                EDataFlow pollFlow = (reqIdxNow == -2) ? eCapture : eRender;
                IMMDevice* defDev = nullptr;
                HRESULT defHr = m_enumerator->GetDefaultAudioEndpoint(pollFlow, eConsole, &defDev);
                if (FAILED(defHr) || !defDev) {
                    defHr = m_enumerator->GetDefaultAudioEndpoint(pollFlow, eMultimedia, &defDev);
                }
                if (SUCCEEDED(defHr) && defDev) {
                    LPWSTR defId = nullptr;
                    if (SUCCEEDED(defDev->GetId(&defId)) && defId) {
                        if (m_deviceId != defId) {
                            if (initEndpoint() && m_audioClient && SUCCEEDED(m_audioClient->Start())) {
                                silentTailLeft = 0;
                            }
                        }
                        CoTaskMemFree(defId);
                    }
                    defDev->Release();
                }
            }
        }

        if (!m_captureClient || !m_pwfx) {
            if (!initEndpoint() || !m_audioClient || FAILED(m_audioClient->Start())) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                continue;
            }
            silentTailLeft = 0;
        }

        if (!m_captureClient || !m_pwfx) continue;

        const bool isFloat = (m_pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
            (m_pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_pwfx)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        const int channels = std::max<int>(1, m_pwfx->nChannels);
        const int bits = m_pwfx->wBitsPerSample;
        const uint32_t sampleRate = m_pwfx->nSamplesPerSec;

        UINT32 packetLength = 0;
        HRESULT hr = m_captureClient->GetNextPacketSize(&packetLength);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED || FAILED(hr)) {
            releaseEndpoint();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (packetLength == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        while (packetLength > 0) {
            BYTE* pData = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;

            hr = m_captureClient->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) releaseEndpoint();
                break;
            }

            if (numFrames > 0) {
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    silentTailLeft = 3;
                    pcmOut.resize(numFrames * 2);

                    if (isFloat) {
                        const float* fData = reinterpret_cast<const float*>(pData);
                        for (UINT32 i = 0; i < numFrames; ++i) {
                            float left = std::max(-1.0f, std::min(1.0f, fData[i * channels + 0]));
                            float right = std::max(-1.0f, std::min(1.0f, (channels > 1) ? fData[i * channels + 1] : left));
                            pcmOut[i * 2 + 0] = static_cast<int16_t>(left * 32767.0f);
                            pcmOut[i * 2 + 1] = static_cast<int16_t>(right * 32767.0f);
                        }
                    } else if (bits == 32) {
                        const int32_t* iData = reinterpret_cast<const int32_t*>(pData);
                        for (UINT32 i = 0; i < numFrames; ++i) {
                            pcmOut[i * 2 + 0] = static_cast<int16_t>(iData[i * channels + 0] >> 16);
                            pcmOut[i * 2 + 1] = static_cast<int16_t>(((channels > 1) ? iData[i * channels + 1] : iData[i * channels + 0]) >> 16);
                        }
                    } else if (bits == 24) {
                        const int stride = channels * 3;
                        for (UINT32 i = 0; i < numFrames; ++i) {
                            const uint8_t* framePtr = pData + i * stride;
                            auto read24 = [](const uint8_t* b) -> int16_t {
                                return static_cast<int16_t>((static_cast<int8_t>(b[2]) << 8) | b[1]);
                            };
                            pcmOut[i * 2 + 0] = read24(framePtr);
                            pcmOut[i * 2 + 1] = (channels > 1) ? read24(framePtr + 3) : pcmOut[i * 2 + 0];
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
                } else if (silentTailLeft > 0) {
                    --silentTailLeft;
                    pcmOut.assign(numFrames * 2, 0);
                    if (m_callback) {
                        m_callback(reinterpret_cast<const uint8_t*>(pcmOut.data()), pcmOut.size() * sizeof(int16_t), sampleRate, 2);
                    }
                }
            }

            m_captureClient->ReleaseBuffer(numFrames);
            if (!m_captureClient) break;
            hr = m_captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) releaseEndpoint();
                break;
            }
        }
    }

    CoUninitialize();
}
