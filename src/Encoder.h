#pragma once

#include "Common.h"
#include "ScreenCapture.h"
#include <d3d11.h>
#include <d3d11_1.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <icodecapi.h>
#include <string>
#include <vector>
#include <atomic>

class Encoder {
public:
    Encoder();
    ~Encoder();

    bool init(ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext,
              uint32_t width, uint32_t height, DXGI_FORMAT srcFormat,
              uint32_t fps, bool isHdrDisplay);
    void cleanup();

    // Encodes from GPU texture (zero-copy GPU VideoProcessor color space conversion) or CPU BGRA fallback.
    bool encodeGpuFrame(ID3D11Texture2D* gpuSrcTex, uint64_t ptsUs,
                        std::vector<uint8_t>& outAnnexB, bool& outIsKeyframe);
    bool encodeBgraCpu(const uint8_t* bgra, uint32_t width, uint32_t height, uint64_t ptsUs,
                       std::vector<uint8_t>& outAnnexB, bool& outIsKeyframe);

    void requestKeyframe() { m_forceKeyframe.store(true); }
    bool hasKeyframeRequest() const { return m_forceKeyframe.load(); }

    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    VideoCodec getCodec() const { return m_codec; }
    ColorProfile getColorProfile() const { return m_colorProfile; }
    bool isHdrActive() const { return m_colorProfile == COLOR_HDR10_PQ; }
    bool isHardwareGpu() const { return m_isHardware; }
    bool isNvidiaEncoder() const { return m_isNvidia; }
    bool hasVideoProcessor() const { return m_hasVideoProcessor; }
    bool isZeroCopyActive() const { return m_useD3dManager && !m_nvDevice; }
    uint32_t getTargetBitrateMbps() const { return ((static_cast<uint64_t>(m_width) * m_height) > 3000000ULL) ? 28u : 14u; }
    const std::string& getEncoderName() const { return m_encoderName; }
    DXGI_FORMAT getSrcFormat() const { return m_srcFormat; }

private:
    bool initEncodeDevice();
    void cleanupEncodeDevice();
    bool initNvUploadTextures(bool useP010);
    void cleanupNvUploadTextures();
    bool uploadYuvToNvTexture(const std::vector<uint8_t>& yuvData, ID3D11Texture2D* dstTex);

    bool initVideoProcessor(DXGI_FORMAT srcFormat, bool useP010);
    void cleanupVideoProcessor();

    bool tryCreateMft(const GUID& outputSubtype, bool wantP010, bool hardwareOnly, bool nvidiaOnly, const char* label);
    bool configureMftTypes(const GUID& outputSubtype, bool wantP010, UINT32 bitrate);
    void cleanupMft();

    bool copyTextureToPackedYuv(ID3D11Texture2D* gpuYuvTex, std::vector<uint8_t>& outYuv);
    static void convertBgraToNv12Studio(const uint8_t* bgra, uint32_t width, uint32_t height, std::vector<uint8_t>& outNv12);

    bool waitAsyncInputReady();
    bool submitSampleToMft(IMFSample* sample, std::vector<uint8_t>& outAnnexB, bool& outIsKeyframe, bool& outInputAccepted);
    bool submitDxgiSurface(ID3D11Texture2D* tex, uint64_t ptsUs,
                           std::vector<uint8_t>& outAnnexB, bool& outIsKeyframe, bool& outInputAccepted);
    bool encodePackedYuv(const std::vector<uint8_t>& yuvData, uint64_t ptsUs,
                         std::vector<uint8_t>& outAnnexB, bool& outIsKeyframe);
    bool collectSyncOutput(std::vector<uint8_t>& outAnnexB, bool& outIsKeyframe);
    bool pumpAsyncEvents(std::vector<uint8_t>& outAnnexB, bool& outIsKeyframe, bool waitOutput);

    ID3D11Device* m_d3dDevice = nullptr;      // non-owning (capture GPU)
    ID3D11DeviceContext* m_d3dContext = nullptr; // non-owning (capture GPU)
    ID3D11Device* m_nvDevice = nullptr;       // owning dedicated NVIDIA GPU when Optimus capture is on iGPU
    ID3D11DeviceContext* m_nvContext = nullptr;
    LUID m_encLuid = {};
    bool m_hasEncLuid = false;
    bool m_encIsNvidia = false;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_fps = 60;
    DXGI_FORMAT m_srcFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    bool m_isHdrSource = false;
    bool m_useP010 = false;

    VideoCodec m_codec = CODEC_H264;
    ColorProfile m_colorProfile = COLOR_SDR_BT709;
    bool m_isHardware = false;
    bool m_isNvidia = false;
    std::string m_encoderName = "Uninitialized";
    std::atomic<bool> m_forceKeyframe{true};

    // D3D11 Video Processor (GPU HDR/SDR color conversion -> NV12/P010 ring)
    static constexpr int kVpRingSize = 4;
    ID3D11VideoDevice* m_videoDevice = nullptr;
    ID3D11VideoContext* m_videoContext = nullptr;
    ID3D11VideoContext1* m_videoContext1 = nullptr;
    ID3D11VideoProcessorEnumerator* m_vpEnum = nullptr;
    ID3D11VideoProcessor* m_vp = nullptr;
    ID3D11Texture2D* m_vpOutTex[kVpRingSize] = {};
    ID3D11VideoProcessorOutputView* m_vpOutView[kVpRingSize] = {};
    int m_vpRingIdx = 0;
    ID3D11Texture2D* m_vpStagingTex = nullptr;
    ID3D11Texture2D* m_nvGpuTex[kVpRingSize] = {};
    ID3D11Texture2D* m_nvUploadTex = nullptr;
    bool m_hasVideoProcessor = false;

    // Zero-Copy D3D11 Media Foundation Manager
    IMFDXGIDeviceManager* m_dxgiManager = nullptr;
    UINT m_dxgiResetToken = 0;
    bool m_useD3dManager = false;

    // Media Foundation Transform
    bool m_mfStarted = false;
    IMFTransform* m_mft = nullptr;
    ICodecAPI* m_codecApi = nullptr;
    IMFMediaEventGenerator* m_eventGen = nullptr;
    bool m_isAsync = false;
    int m_needInputCount = 0;
    DWORD m_inStreamId = 0;
    DWORD m_outStreamId = 0;
    DWORD m_outBufSize = 0;

    std::vector<uint8_t> m_yuvBuffer;
    std::vector<uint8_t> m_seqHeader; // Cached SPS/PPS(/VPS) if needed
};
