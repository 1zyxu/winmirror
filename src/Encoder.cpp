#include "Encoder.h"
#include <codecapi.h>
#include <mferror.h>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <cstring>

// Adapter LUID attribute for MFTEnum2 (defined locally for MinGW + Windows SDK portability)
static const GUID kMFT_ENUM_ADAPTER_LUID = {
    0x1d39518c, 0xe220, 0x4da8, { 0xa0, 0x7f, 0xba, 0x17, 0x25, 0x52, 0xd6, 0xb1 }
};

using PFN_MFTEnum2 = HRESULT(WINAPI*)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*,
                                     const MFT_REGISTER_TYPE_INFO*, IMFAttributes*,
                                     IMFActivate***, UINT32*);

static std::string wideToUtf8(const WCHAR* w) {
    if (!w || !*w) return {};
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 1) return {};
    std::string out(utf8Len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], utf8Len, nullptr, nullptr);
    return out;
}

static std::string readMftAttrString(IMFActivate* act, const GUID& key) {
    if (!act) return {};
    WCHAR* str = nullptr;
    UINT32 len = 0;
    std::string out;
    if (SUCCEEDED(act->GetAllocatedString(key, &str, &len)) && str) {
        out = wideToUtf8(str);
        CoTaskMemFree(str);
    }
    return out;
}

static bool containsInsensitive(const std::string& haystack, const char* needle) {
    if (!needle || !*needle) return false;
    std::string h = haystack, n = needle;
    for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h.find(n) != std::string::npos;
}

static bool isNvidiaMftCandidate(IMFActivate* act, std::string& outFriendlyName) {
    outFriendlyName = readMftAttrString(act, MFT_FRIENDLY_NAME_Attribute);
    std::string vendorId = readMftAttrString(act, MFT_ENUM_HARDWARE_VENDOR_ID_Attribute);
    return containsInsensitive(vendorId, "10de") || containsInsensitive(outFriendlyName, "nvidia");
}

static const DXGI_FORMAT kTryVideoProcInFormats[] = {
    DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_R10G10B10A2_UNORM,
    DXGI_FORMAT_B8G8R8A8_UNORM,
    DXGI_FORMAT_R8G8B8A8_UNORM
};

static bool setCodecBool(ICodecAPI* api, const GUID& key, bool val) {
    if (!api) return false;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = val ? VARIANT_TRUE : VARIANT_FALSE;
    HRESULT hr = api->SetValue(&key, &v);
    VariantClear(&v);
    return SUCCEEDED(hr);
}

static bool setCodecUint32(ICodecAPI* api, const GUID& key, UINT32 val) {
    if (!api) return false;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = val;
    HRESULT hr = api->SetValue(&key, &v);
    VariantClear(&v);
    return SUCCEEDED(hr);
}

static void applyColorAttributes(IMFAttributes* attrs, bool isHdr10Pq) {
    if (!attrs) return;
    if (isHdr10Pq) {
        attrs->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT2020_10);
        attrs->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT2020);
        attrs->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_2084);
        attrs->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    } else {
        attrs->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        attrs->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        attrs->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        attrs->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    }
}

Encoder::Encoder() = default;

Encoder::~Encoder() {
    cleanup();
}

bool Encoder::initEncodeDevice() {
    cleanupEncodeDevice();

    bool capIsNvidia = false;
    if (m_d3dDevice) {
        IDXGIDevice* dxgiDev = nullptr;
        if (SUCCEEDED(m_d3dDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev))) && dxgiDev) {
            IDXGIAdapter* adp = nullptr;
            if (SUCCEEDED(dxgiDev->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&adp))) && adp) {
                DXGI_ADAPTER_DESC desc = {};
                if (SUCCEEDED(adp->GetDesc(&desc))) {
                    m_encLuid = desc.AdapterLuid;
                    m_hasEncLuid = true;
                    capIsNvidia = (desc.VendorId == 0x10DE);
                    m_encIsNvidia = capIsNvidia;
                }
                adp->Release();
            }
            dxgiDev->Release();
        }
    }

    // Optimus / hybrid PC: capture runs on display-owning iGPU, so spin up a dedicated NVIDIA device for NVENC
    if (!capIsNvidia) {
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory) {
            for (UINT idx = 0; ; ++idx) {
                IDXGIAdapter1* cand = nullptr;
                if (factory->EnumAdapters1(idx, &cand) == DXGI_ERROR_NOT_FOUND || !cand) break;

                DXGI_ADAPTER_DESC1 ad = {};
                cand->GetDesc1(&ad);
                if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && ad.VendorId == 0x10DE) {
                    D3D_FEATURE_LEVEL levels[] = {
                        D3D_FEATURE_LEVEL_11_1,
                        D3D_FEATURE_LEVEL_11_0,
                        D3D_FEATURE_LEVEL_10_1,
                        D3D_FEATURE_LEVEL_10_0
                    };
                    D3D_FEATURE_LEVEL chosen;
                    HRESULT hr = D3D11CreateDevice(
                        cand, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                        &m_nvDevice, &chosen, &m_nvContext);
                    if (FAILED(hr)) {
                        hr = D3D11CreateDevice(
                            cand, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                            &m_nvDevice, &chosen, &m_nvContext);
                    }
                    if (SUCCEEDED(hr) && m_nvDevice && m_nvContext) {
                        ID3D10Multithread* mt = nullptr;
                        if (SUCCEEDED(m_nvDevice->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&mt)))) {
                            mt->SetMultithreadProtected(TRUE);
                            mt->Release();
                        }
                        IDXGIDevice* nvDxgi = nullptr;
                        if (SUCCEEDED(m_nvDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&nvDxgi))) && nvDxgi) {
                            nvDxgi->SetGPUThreadPriority(7);
                            nvDxgi->Release();
                        }
                        m_encLuid = ad.AdapterLuid;
                        m_hasEncLuid = true;
                        m_encIsNvidia = true;
                        std::cout << "[Encoder] Dedicated NVIDIA Encode GPU: " << wideToUtf8(ad.Description)
                                  << " (Optimus bridge active)" << std::endl;
                        cand->Release();
                        break;
                    }
                }
                cand->Release();
            }
            factory->Release();
        }
    }

    ID3D11Device* activeEncDev = m_nvDevice ? m_nvDevice : m_d3dDevice;
    if (activeEncDev) {
        if (SUCCEEDED(MFCreateDXGIDeviceManager(&m_dxgiResetToken, &m_dxgiManager)) && m_dxgiManager) {
            if (FAILED(m_dxgiManager->ResetDevice(activeEncDev, m_dxgiResetToken))) {
                m_dxgiManager->Release();
                m_dxgiManager = nullptr;
            }
        }
    }
    return true;
}

void Encoder::cleanupEncodeDevice() {
    cleanupNvUploadTextures();
    if (m_dxgiManager) {
        m_dxgiManager->Release();
        m_dxgiManager = nullptr;
    }
    if (m_nvContext) {
        m_nvContext->Release();
        m_nvContext = nullptr;
    }
    if (m_nvDevice) {
        m_nvDevice->Release();
        m_nvDevice = nullptr;
    }
    m_encLuid = {};
    m_hasEncLuid = false;
    m_encIsNvidia = false;
}

bool Encoder::initNvUploadTextures(bool useP010) {
    cleanupNvUploadTextures();
    if (!m_nvDevice) return true;

    DXGI_FORMAT fmt = useP010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    D3D11_TEXTURE2D_DESC gpuDesc = {};
    gpuDesc.Width = m_width;
    gpuDesc.Height = m_height;
    gpuDesc.MipLevels = 1;
    gpuDesc.ArraySize = 1;
    gpuDesc.Format = fmt;
    gpuDesc.SampleDesc.Count = 1;
    gpuDesc.Usage = D3D11_USAGE_DEFAULT;
    gpuDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    for (int i = 0; i < kVpRingSize; ++i) {
        HRESULT hr = m_nvDevice->CreateTexture2D(&gpuDesc, nullptr, &m_nvGpuTex[i]);
        if (FAILED(hr) || !m_nvGpuTex[i]) {
            gpuDesc.BindFlags = 0;
            hr = m_nvDevice->CreateTexture2D(&gpuDesc, nullptr, &m_nvGpuTex[i]);
        }
        if (FAILED(hr) || !m_nvGpuTex[i]) {
            cleanupNvUploadTextures();
            return false;
        }
    }

    D3D11_TEXTURE2D_DESC stDesc = gpuDesc;
    stDesc.Usage = D3D11_USAGE_STAGING;
    stDesc.BindFlags = 0;
    stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_nvDevice->CreateTexture2D(&stDesc, nullptr, &m_nvUploadTex)) || !m_nvUploadTex) {
        cleanupNvUploadTextures();
        return false;
    }
    return true;
}

void Encoder::cleanupNvUploadTextures() {
    if (m_nvUploadTex) { m_nvUploadTex->Release(); m_nvUploadTex = nullptr; }
    for (int i = 0; i < kVpRingSize; ++i) {
        if (m_nvGpuTex[i]) { m_nvGpuTex[i]->Release(); m_nvGpuTex[i] = nullptr; }
    }
}

bool Encoder::mapStagingRead(ID3D11Texture2D* stagingTex, D3D11_MAPPED_SUBRESOURCE& outMap) {
    if (!stagingTex || !m_d3dContext) return false;
    auto waitStart = std::chrono::steady_clock::now();
    for (;;) {
        HRESULT hr = m_d3dContext->Map(stagingTex, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &outMap);
        if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
            return SUCCEEDED(hr);
        }
        auto elapsed = std::chrono::steady_clock::now() - waitStart;
        if (elapsed > std::chrono::milliseconds(16)) {
            return SUCCEEDED(m_d3dContext->Map(stagingTex, 0, D3D11_MAP_READ, 0, &outMap));
        }
        if (elapsed < std::chrono::microseconds(500)) {
            Sleep(0);
        } else {
            Sleep(1);
        }
    }
}

bool Encoder::copyStagedYuvToNvTexture(ID3D11Texture2D* gpuYuvTex, ID3D11Texture2D* dstTex) {
    if (!gpuYuvTex || !m_vpStagingTex || !m_d3dContext || !m_nvContext || !m_nvUploadTex || !dstTex) return false;

    m_d3dContext->CopyResource(m_vpStagingTex, gpuYuvTex);

    D3D11_MAPPED_SUBRESOURCE srcMap = {};
    if (!mapStagingRead(m_vpStagingTex, srcMap)) return false;

    D3D11_MAPPED_SUBRESOURCE dstMap = {};
    if (FAILED(m_nvContext->Map(m_nvUploadTex, 0, D3D11_MAP_WRITE, 0, &dstMap))) {
        m_d3dContext->Unmap(m_vpStagingTex, 0);
        return false;
    }

    const size_t rowBytes = m_useP010 ? (static_cast<size_t>(m_width) * 2) : static_cast<size_t>(m_width);
    const size_t totalRows = static_cast<size_t>(m_height) + m_height / 2;

    if (srcMap.RowPitch >= rowBytes && dstMap.RowPitch >= rowBytes) {
        if (srcMap.RowPitch == rowBytes && dstMap.RowPitch == rowBytes) {
            memcpy(dstMap.pData, srcMap.pData, totalRows * rowBytes);
        } else {
            const uint8_t* src = static_cast<const uint8_t*>(srcMap.pData);
            uint8_t* dst = static_cast<uint8_t*>(dstMap.pData);
            for (size_t y = 0; y < totalRows; ++y) {
                memcpy(dst, src, rowBytes);
                src += srcMap.RowPitch;
                dst += dstMap.RowPitch;
            }
        }
    }

    m_nvContext->Unmap(m_nvUploadTex, 0);
    m_d3dContext->Unmap(m_vpStagingTex, 0);
    m_nvContext->CopyResource(dstTex, m_nvUploadTex);
    m_nvContext->Flush();
    return true;
}

bool Encoder::init(ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dContext,
                   uint32_t width, uint32_t height, DXGI_FORMAT srcFormat,
                   uint32_t fps, bool isHdrDisplay) {
    cleanup();

    m_d3dDevice = d3dDevice;
    m_d3dContext = d3dContext;
    if (m_d3dDevice) m_d3dDevice->AddRef();
    if (m_d3dContext) m_d3dContext->AddRef();
    m_width = width & ~1u;   // YUV 4:2:0 requires even dimensions
    m_height = height & ~1u;
    m_fps = fps > 0 ? fps : 60;
    m_srcFormat = srcFormat;
    m_isHdrSource = isHdrDisplay || (srcFormat == DXGI_FORMAT_R16G16B16A16_FLOAT);

    if (m_width == 0 || m_height == 0) return false;

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (SUCCEEDED(hr)) {
        m_mfStarted = true;
    } else {
        std::cerr << "[Encoder] MFStartup failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    initEncodeDevice();

    struct EncodeAttempt {
        GUID subtype;
        bool p010;
        bool hw;
        bool nvidiaOnly;
        const char* label;
    };
    std::vector<EncodeAttempt> plan;
    if (m_encIsNvidia) {
        if (m_isHdrSource) {
            plan.push_back({ MFVideoFormat_HEVC, true,  true, true, "NVENC HEVC 10-bit HDR10 (GPU)" });
        }
        plan.push_back({ MFVideoFormat_HEVC, false, true, true, "NVENC HEVC 8-bit (GPU)" });
        plan.push_back({ MFVideoFormat_H264, false, true, true, "NVENC H.264 (GPU)" });
    }
    if (m_isHdrSource) {
        plan.push_back({ MFVideoFormat_HEVC, true,  true, false, "HEVC 10-bit HDR10 (GPU)" });
    }
    plan.push_back({ MFVideoFormat_HEVC, false, true,  false, "HEVC 8-bit (GPU)" });
    plan.push_back({ MFVideoFormat_H264, false, true,  false, "H.264 (GPU Hardware)" });
    plan.push_back({ MFVideoFormat_H264, false, false, false, "H.264 (Windows MFT)" });

    int vpMode = -1; // -1 = none, 0 = 8-bit NV12, 1 = 10-bit P010
    for (const auto& step : plan) {
        if (step.p010 && !m_d3dDevice) continue;

        if (!step.nvidiaOnly && m_nvDevice) {
            // Dedicated NVIDIA GPU had no working NVENC MFT; revert DXGI manager to capture GPU for iGPU/SW fallback
            cleanupNvUploadTextures();
            if (m_nvContext) { m_nvContext->Release(); m_nvContext = nullptr; }
            if (m_nvDevice) { m_nvDevice->Release(); m_nvDevice = nullptr; }
            m_encIsNvidia = false;
            m_hasEncLuid = false;
            if (m_dxgiManager && m_d3dDevice) {
                if (FAILED(m_dxgiManager->ResetDevice(m_d3dDevice, m_dxgiResetToken))) {
                    m_dxgiManager->Release();
                    m_dxgiManager = nullptr;
                }
            }
        }

        int targetMode = step.p010 ? 1 : 0;
        if (vpMode != targetMode) {
            cleanupVideoProcessor();
            cleanupNvUploadTextures();
            vpMode = -1;
            if (m_d3dDevice) {
                bool vpOk = initVideoProcessor(srcFormat, step.p010);
                if (step.p010 && !vpOk) continue;
                if (vpOk && !initNvUploadTextures(step.p010)) {
                    if (step.p010) { cleanupVideoProcessor(); continue; }
                }
            }
            vpMode = targetMode;
        }

        m_useP010 = step.p010;
        m_colorProfile = step.p010 ? COLOR_HDR10_PQ : COLOR_SDR_BT709;
        if (tryCreateMft(step.subtype, step.p010, step.hw, step.nvidiaOnly, step.label)) {
            m_codec = (step.subtype == MFVideoFormat_HEVC) ? CODEC_HEVC : CODEC_H264;
            std::cout << "[Encoder] Initialized: " << m_encoderName << " " << m_width << "x" << m_height << std::endl;
            return true;
        }
    }

    cleanup();
    return false;
}

void Encoder::cleanup() {
    cleanupMft();
    cleanupVideoProcessor();
    cleanupEncodeDevice();
    if (m_d3dContext) { m_d3dContext->Release(); m_d3dContext = nullptr; }
    if (m_d3dDevice) { m_d3dDevice->Release(); m_d3dDevice = nullptr; }
    if (m_mfStarted) {
        MFShutdown();
        m_mfStarted = false;
    }
}

bool Encoder::initVideoProcessor(DXGI_FORMAT srcFormat, bool useP010) {
    cleanupVideoProcessor();
    if (!m_d3dDevice || !m_d3dContext) return false;

    HRESULT hr = m_d3dDevice->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(&m_videoDevice));
    if (FAILED(hr) || !m_videoDevice) return false;

    hr = m_d3dContext->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void**>(&m_videoContext));
    if (FAILED(hr) || !m_videoContext) { cleanupVideoProcessor(); return false; }

    m_d3dContext->QueryInterface(__uuidof(ID3D11VideoContext1), reinterpret_cast<void**>(&m_videoContext1));

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputFrameRate.Numerator = m_fps;
    contentDesc.InputFrameRate.Denominator = 1;
    contentDesc.InputWidth = m_width;
    contentDesc.InputHeight = m_height;
    contentDesc.OutputFrameRate.Numerator = m_fps;
    contentDesc.OutputFrameRate.Denominator = 1;
    contentDesc.OutputWidth = m_width;
    contentDesc.OutputHeight = m_height;
    contentDesc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;

    hr = m_videoDevice->CreateVideoProcessorEnumerator(&contentDesc, &m_vpEnum);
    if (FAILED(hr) || !m_vpEnum) { cleanupVideoProcessor(); return false; }

    DXGI_FORMAT outFmt = useP010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    UINT supportFlags = 0;
    if (FAILED(m_vpEnum->CheckVideoProcessorFormat(outFmt, &supportFlags)) ||
        !(supportFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        cleanupVideoProcessor();
        return false;
    }
    if (FAILED(m_vpEnum->CheckVideoProcessorFormat(srcFormat, &supportFlags)) ||
        !(supportFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
        cleanupVideoProcessor();
        return false;
    }

    hr = m_videoDevice->CreateVideoProcessor(m_vpEnum, 0, &m_vp);
    if (FAILED(hr) || !m_vp) { cleanupVideoProcessor(); return false; }

    D3D11_TEXTURE2D_DESC outDesc = {};
    outDesc.Width = m_width;
    outDesc.Height = m_height;
    outDesc.MipLevels = 1;
    outDesc.ArraySize = 1;
    outDesc.Format = outFmt;
    outDesc.SampleDesc.Count = 1;
    outDesc.Usage = D3D11_USAGE_DEFAULT;
    outDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    m_vpRingIdx = 0;
    for (int i = 0; i < kVpRingSize; ++i) {
        hr = m_d3dDevice->CreateTexture2D(&outDesc, nullptr, &m_vpOutTex[i]);
        if (FAILED(hr) || !m_vpOutTex[i]) {
            outDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
            hr = m_d3dDevice->CreateTexture2D(&outDesc, nullptr, &m_vpOutTex[i]);
        }
        if (FAILED(hr) || !m_vpOutTex[i]) { cleanupVideoProcessor(); return false; }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovDesc = {};
        ovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ovDesc.Texture2D.MipSlice = 0;
        hr = m_videoDevice->CreateVideoProcessorOutputView(m_vpOutTex[i], m_vpEnum, &ovDesc, &m_vpOutView[i]);
        if (FAILED(hr) || !m_vpOutView[i]) { cleanupVideoProcessor(); return false; }
    }

    outDesc.Usage = D3D11_USAGE_STAGING;
    outDesc.BindFlags = 0;
    outDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = m_d3dDevice->CreateTexture2D(&outDesc, nullptr, &m_vpStagingTex);
    if (FAILED(hr) || !m_vpStagingTex) { cleanupVideoProcessor(); return false; }

    // Disable auto contrast/sharpening so text & game UI stay clean
    m_videoContext->VideoProcessorSetStreamAutoProcessingMode(m_vp, 0, FALSE);

    if (m_videoContext1) {
        DXGI_COLOR_SPACE_TYPE inSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        if (srcFormat == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            inSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709; // Windows 11 scRGB linear HDR
        } else if (srcFormat == DXGI_FORMAT_R10G10B10A2_UNORM && m_isHdrSource) {
            inSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        }
        DXGI_COLOR_SPACE_TYPE outSpace = useP010
            ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020
            : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;

        m_videoContext1->VideoProcessorSetStreamColorSpace1(m_vp, 0, inSpace);
        m_videoContext1->VideoProcessorSetOutputColorSpace1(m_vp, outSpace);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs = {};
        cs.YCbCr_Matrix = 1; // BT.709
        m_videoContext->VideoProcessorSetOutputColorSpace(m_vp, &cs);
    }

    m_hasVideoProcessor = true;
    return true;
}

void Encoder::cleanupVideoProcessor() {
    m_hasVideoProcessor = false;
    if (m_vpStagingTex) { m_vpStagingTex->Release(); m_vpStagingTex = nullptr; }
    for (int i = 0; i < kVpRingSize; ++i) {
        if (m_vpOutView[i]) { m_vpOutView[i]->Release(); m_vpOutView[i] = nullptr; }
        if (m_vpOutTex[i]) { m_vpOutTex[i]->Release(); m_vpOutTex[i] = nullptr; }
    }
    m_vpRingIdx = 0;
    if (m_vp) { m_vp->Release(); m_vp = nullptr; }
    if (m_vpEnum) { m_vpEnum->Release(); m_vpEnum = nullptr; }
    if (m_videoContext1) { m_videoContext1->Release(); m_videoContext1 = nullptr; }
    if (m_videoContext) { m_videoContext->Release(); m_videoContext = nullptr; }
    if (m_videoDevice) { m_videoDevice->Release(); m_videoDevice = nullptr; }
}

bool Encoder::configureMftTypes(const GUID& outputSubtype, bool wantP010, UINT32 bitrate) {
    auto applyRtProps = [&]() {
        if (!m_codecApi) return;
        setCodecBool(m_codecApi, CODECAPI_AVLowLatencyMode, true);
        setCodecBool(m_codecApi, CODECAPI_AVEncCommonRealTime, true);
        setCodecUint32(m_codecApi, CODECAPI_AVEncCommonQualityVsSpeed, 0);
        setCodecUint32(m_codecApi, CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        setCodecUint32(m_codecApi, CODECAPI_AVEncCommonMeanBitRate, bitrate);
        setCodecUint32(m_codecApi, CODECAPI_AVEncCommonMaxBitRate, bitrate + (bitrate / 4));
        setCodecUint32(m_codecApi, CODECAPI_AVEncCommonBufferSize, bitrate / 4);
        setCodecUint32(m_codecApi, CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        setCodecUint32(m_codecApi, CODECAPI_AVEncMPVGOPSize, m_fps * 2);
    };
    applyRtProps();

    // 1. Configure Output Type FIRST (full profile+color -> color attributes -> minimal -> driver available type)
    bool outOk = false;
    for (int mode = 0; mode < 3 && !outOk; ++mode) {
        IMFMediaType* outType = nullptr;
        if (FAILED(MFCreateMediaType(&outType)) || !outType) continue;
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, outputSubtype);
        outType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, m_width, m_height);
        MFSetAttributeRatio(outType, MF_MT_FRAME_RATE, m_fps, 1);
        MFSetAttributeRatio(outType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (mode == 0) {
            outType->SetUINT32(MF_MT_MPEG2_PROFILE,
                (outputSubtype == MFVideoFormat_HEVC)
                    ? (wantP010 ? eAVEncH265VProfile_Main_420_10 : eAVEncH265VProfile_Main_420_8)
                    : eAVEncH264VProfile_High);
        }
        if (mode <= 1) {
            applyColorAttributes(outType, wantP010);
        }
        outOk = SUCCEEDED(m_mft->SetOutputType(m_outStreamId, outType, 0));
        outType->Release();
    }
    for (DWORD k = 0; !outOk; ++k) {
        IMFMediaType* availOut = nullptr;
        if (FAILED(m_mft->GetOutputAvailableType(m_outStreamId, k, &availOut)) || !availOut) break;
        GUID st = GUID_NULL;
        availOut->GetGUID(MF_MT_SUBTYPE, &st);
        if (st == outputSubtype) {
            availOut->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
            MFSetAttributeSize(availOut, MF_MT_FRAME_SIZE, m_width, m_height);
            MFSetAttributeRatio(availOut, MF_MT_FRAME_RATE, m_fps, 1);
            MFSetAttributeRatio(availOut, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            availOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            applyColorAttributes(availOut, wantP010);
            outOk = SUCCEEDED(m_mft->SetOutputType(m_outStreamId, availOut, 0));
        }
        availOut->Release();
    }
    if (!outOk) return false;

    applyRtProps();

    // 2. Configure Input Type SECOND (stride+color -> color attributes -> minimal -> driver available type)
    const GUID inSubtype = wantP010 ? MFVideoFormat_P010 : MFVideoFormat_NV12;
    bool inOk = false;
    for (int mode = 0; mode < 3 && !inOk; ++mode) {
        IMFMediaType* inType = nullptr;
        if (FAILED(MFCreateMediaType(&inType)) || !inType) continue;
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, inSubtype);
        MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, m_width, m_height);
        MFSetAttributeRatio(inType, MF_MT_FRAME_RATE, m_fps, 1);
        MFSetAttributeRatio(inType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (mode == 0) {
            inType->SetUINT32(MF_MT_DEFAULT_STRIDE, wantP010 ? (m_width * 2) : m_width);
        }
        if (mode <= 1) {
            applyColorAttributes(inType, wantP010);
        }
        inOk = SUCCEEDED(m_mft->SetInputType(m_inStreamId, inType, 0));
        inType->Release();
    }
    for (DWORD k = 0; !inOk; ++k) {
        IMFMediaType* availIn = nullptr;
        if (FAILED(m_mft->GetInputAvailableType(m_inStreamId, k, &availIn)) || !availIn) break;
        GUID st = GUID_NULL;
        availIn->GetGUID(MF_MT_SUBTYPE, &st);
        if (st == inSubtype) {
            MFSetAttributeSize(availIn, MF_MT_FRAME_SIZE, m_width, m_height);
            MFSetAttributeRatio(availIn, MF_MT_FRAME_RATE, m_fps, 1);
            MFSetAttributeRatio(availIn, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            availIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            applyColorAttributes(availIn, wantP010);
            inOk = SUCCEEDED(m_mft->SetInputType(m_inStreamId, availIn, 0));
        }
        availIn->Release();
    }
    return inOk;
}

bool Encoder::tryCreateMft(const GUID& outputSubtype, bool wantP010, bool hardwareOnly, bool nvidiaOnly, const char* label) {
    cleanupMft();
    if (nvidiaOnly && !hardwareOnly) return false;

    MFT_REGISTER_TYPE_INFO outInfo = { MFMediaType_Video, outputSubtype };
    IMFActivate** ppActivate = nullptr;
    UINT32 count = 0;

    if (hardwareOnly) {
        // On multi-GPU/Optimus systems, target the exact NVIDIA LUID via MFTEnum2 first
        if (m_hasEncLuid) {
            auto pEnum2 = reinterpret_cast<PFN_MFTEnum2>(GetProcAddress(GetModuleHandleW(L"mfplat.dll"), "MFTEnum2"));
            IMFAttributes* enumAttr = nullptr;
            if (pEnum2 && SUCCEEDED(MFCreateAttributes(&enumAttr, 1)) && enumAttr) {
                enumAttr->SetBlob(kMFT_ENUM_ADAPTER_LUID, reinterpret_cast<const BYTE*>(&m_encLuid), sizeof(LUID));
                pEnum2(MFT_CATEGORY_VIDEO_ENCODER,
                       MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                       nullptr, &outInfo, enumAttr, &ppActivate, &count);
                enumAttr->Release();
            }
        }
        auto hasNvidiaInList = [&](IMFActivate** list, UINT32 n) {
            std::string dummy;
            for (UINT32 i = 0; i < n; ++i) {
                if (isNvidiaMftCandidate(list[i], dummy)) return true;
            }
            return false;
        };
        if (count == 0 || (nvidiaOnly && !hasNvidiaInList(ppActivate, count))) {
            for (UINT32 j = 0; j < count; ++j) ppActivate[j]->Release();
            if (ppActivate) { CoTaskMemFree(ppActivate); ppActivate = nullptr; }
            count = 0;
            // Enumerate all hardware MFTs without SORTANDFILTER so primary-adapter merit cannot hide NVIDIA
            MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                      MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT,
                      nullptr, &outInfo, &ppActivate, &count);
            if (count == 0) {
                MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                          MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                          nullptr, &outInfo, &ppActivate, &count);
            }
        }
    } else {
        MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                  MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                  nullptr, &outInfo, &ppActivate, &count);
    }

    if (count == 0 || !ppActivate) {
        if (ppActivate) CoTaskMemFree(ppActivate);
        return false;
    }

    // Sort hardware MFTs so NVIDIA NVENC is always tried first
    std::vector<std::pair<int, UINT32>> candidates;
    candidates.reserve(count);
    for (UINT32 idx = 0; idx < count; ++idx) {
        std::string name;
        bool isNv = isNvidiaMftCandidate(ppActivate[idx], name);
        if (nvidiaOnly && !isNv) continue;
        int rank = isNv ? 0 : ((containsInsensitive(name, "amd") || containsInsensitive(name, "intel")) ? 1 : 2);
        candidates.push_back({ rank, idx });
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    uint64_t pixels = static_cast<uint64_t>(m_width) * m_height;
    UINT32 bitrate = wantP010
        ? ((pixels > 3000000ULL) ? 32000000u : 18000000u)
        : ((pixels > 3000000ULL) ? 28000000u : 14000000u);

    for (const auto& cand : candidates) {
        UINT32 idx = cand.second;
        std::string rawFriendly;
        bool isNvidiaCand = isNvidiaMftCandidate(ppActivate[idx], rawFriendly);

        cleanupMft();
        if (FAILED(ppActivate[idx]->ActivateObject(__uuidof(IMFTransform), reinterpret_cast<void**>(&m_mft))) || !m_mft) {
            continue;
        }

        // Handle Async MFT unlock and D3D11 zero-copy binding if hardware driver supports it
        IMFAttributes* mftAttr = nullptr;
        UINT32 isAsync = 0;
        UINT32 d3d11Aware = 0;
        UINT32 d3dAware = 0;
        if (SUCCEEDED(m_mft->GetAttributes(&mftAttr)) && mftAttr) {
            mftAttr->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
            mftAttr->GetUINT32(MF_SA_D3D11_AWARE, &d3d11Aware);
            mftAttr->GetUINT32(MF_SA_D3D_AWARE, &d3dAware);
            if (isAsync) {
                if (FAILED(mftAttr->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE))) {
                    mftAttr->Release();
                    continue;
                }
                m_isAsync = true;
                m_mft->QueryInterface(__uuidof(IMFMediaEventGenerator), reinterpret_cast<void**>(&m_eventGen));
                if (!m_eventGen) {
                    mftAttr->Release();
                    continue;
                }
            }
            mftAttr->Release();
        }

        DWORD inCount = 0, outCount = 0;
        if (SUCCEEDED(m_mft->GetStreamCount(&inCount, &outCount)) && inCount > 0 && outCount > 0) {
            DWORD inIds[8] = {}, outIds[8] = {};
            if (SUCCEEDED(m_mft->GetStreamIDs(inCount, inIds, outCount, outIds))) {
                m_inStreamId = inIds[0];
                m_outStreamId = outIds[0];
            } else {
                m_inStreamId = 0;
                m_outStreamId = 0;
            }
        }

        bool hasValidSurf = m_nvDevice ? (m_nvGpuTex[0] != nullptr) : m_hasVideoProcessor;
        if (hardwareOnly && (d3d11Aware || d3dAware) && m_dxgiManager && hasValidSurf) {
            if (SUCCEEDED(m_mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(m_dxgiManager)))) {
                m_useD3dManager = true;
            }
        }

        m_mft->QueryInterface(__uuidof(ICodecAPI), reinterpret_cast<void**>(&m_codecApi));

        if (!configureMftTypes(outputSubtype, wantP010, bitrate)) {
            if (isNvidiaCand) {
                std::cerr << "[Encoder] Skipped candidate '" << rawFriendly << "' (media type negotiation failed)" << std::endl;
            }
            continue;
        }

        MFT_OUTPUT_STREAM_INFO outStreamInfo = {};
        if (SUCCEEDED(m_mft->GetOutputStreamInfo(m_outStreamId, &outStreamInfo))) {
            m_outBufSize = outStreamInfo.cbSize > 0 ? outStreamInfo.cbSize : (1024 * 1024);
        } else {
            m_outBufSize = 1024 * 1024;
        }

        // Extract optional sequence header if provided before streaming starts
        IMFMediaType* curOut = nullptr;
        if (SUCCEEDED(m_mft->GetOutputCurrentType(m_outStreamId, &curOut)) && curOut) {
            UINT32 hdrLen = 0;
            if (SUCCEEDED(curOut->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &hdrLen)) && hdrLen > 0) {
                m_seqHeader.resize(hdrLen);
                curOut->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, m_seqHeader.data(), hdrLen, &hdrLen);
            }
            curOut->Release();
        }

        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        m_isHardware = hardwareOnly;
        m_isNvidia = isNvidiaCand;
        std::string hwName = !rawFriendly.empty() ? (std::string(label) + " [" + rawFriendly + "]") : label;
        if (m_useD3dManager) {
            hwName += m_nvDevice ? " (Optimus NVIDIA VRAM)" : " (D3D11 Zero-Copy)";
        }
        m_encoderName = hwName;
        m_forceKeyframe.store(true);

        for (UINT32 j = 0; j < count; ++j) ppActivate[j]->Release();
        CoTaskMemFree(ppActivate);
        return true;
    }

    cleanupMft();
    for (UINT32 j = 0; j < count; ++j) ppActivate[j]->Release();
    CoTaskMemFree(ppActivate);
    return false;
}

void Encoder::cleanupMft() {
    if (m_mft) {
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    if (m_eventGen) { m_eventGen->Release(); m_eventGen = nullptr; }
    if (m_codecApi) { m_codecApi->Release(); m_codecApi = nullptr; }
    if (m_mft) { m_mft->Release(); m_mft = nullptr; }
    m_isAsync = false;
    m_useD3dManager = false;
    m_isHardware = false;
    m_isNvidia = false;
    m_needInputCount = 0;
    m_seqHeader.clear();
    m_readyOutputs.clear();
}

bool Encoder::copyTextureToPackedYuv(ID3D11Texture2D* gpuYuvTex, std::vector<uint8_t>& outYuv) {
    if (!gpuYuvTex || !m_vpStagingTex || !m_d3dContext) return false;

    m_d3dContext->CopyResource(m_vpStagingTex, gpuYuvTex);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (!mapStagingRead(m_vpStagingTex, mapped)) {
        return false;
    }

    size_t rowBytes = m_useP010 ? (static_cast<size_t>(m_width) * 2) : static_cast<size_t>(m_width);
    size_t yRows = m_height;
    size_t uvRows = m_height / 2;
    size_t totalBytes = (yRows + uvRows) * rowBytes;

    if (outYuv.size() != totalBytes) outYuv.resize(totalBytes);

    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* dst = outYuv.data();

    for (size_t y = 0; y < yRows + uvRows; ++y) {
        memcpy(dst, src, rowBytes);
        src += mapped.RowPitch;
        dst += rowBytes;
    }

    m_d3dContext->Unmap(m_vpStagingTex, 0);
    return true;
}

void Encoder::convertBgraToNv12Studio(const uint8_t* bgra, uint32_t width, uint32_t height, std::vector<uint8_t>& outNv12) {
    // Exact Rec.709 Studio Range (16..235 Y, 16..240 UV) fixed-point conversion
    size_t ySize = static_cast<size_t>(width) * height;
    size_t totalSize = ySize + ySize / 2;
    if (outNv12.size() != totalSize) outNv12.resize(totalSize);

    uint8_t* yPlane = outNv12.data();
    uint8_t* uvPlane = yPlane + ySize;
    size_t bgraStride = static_cast<size_t>(width) * 4;

    for (uint32_t y = 0; y < height; y += 2) {
        const uint8_t* row0 = bgra + y * bgraStride;
        const uint8_t* row1 = row0 + bgraStride;
        uint8_t* y0 = yPlane + static_cast<size_t>(y) * width;
        uint8_t* y1 = y0 + width;
        uint8_t* uv = uvPlane + static_cast<size_t>(y / 2) * width;

        for (uint32_t x = 0; x < width; x += 2) {
            int b00 = row0[0], g00 = row0[1], r00 = row0[2];
            int b01 = row0[4], g01 = row0[5], r01 = row0[6];
            int b10 = row1[0], g10 = row1[1], r10 = row1[2];
            int b11 = row1[4], g11 = row1[5], r11 = row1[6];

            y0[0] = static_cast<uint8_t>(((47 * r00 + 157 * g00 + 16 * b00 + 128) >> 8) + 16);
            y0[1] = static_cast<uint8_t>(((47 * r01 + 157 * g01 + 16 * b01 + 128) >> 8) + 16);
            y1[0] = static_cast<uint8_t>(((47 * r10 + 157 * g10 + 16 * b10 + 128) >> 8) + 16);
            y1[1] = static_cast<uint8_t>(((47 * r11 + 157 * g11 + 16 * b11 + 128) >> 8) + 16);

            int rAvg = (r00 + r01 + r10 + r11 + 2) >> 2;
            int gAvg = (g00 + g01 + g10 + g11 + 2) >> 2;
            int bAvg = (b00 + b01 + b10 + b11 + 2) >> 2;

            uv[0] = static_cast<uint8_t>(((-26 * rAvg - 87 * gAvg + 112 * bAvg + 128) >> 8) + 128);
            uv[1] = static_cast<uint8_t>(((112 * rAvg - 102 * gAvg - 10 * bAvg + 128) >> 8) + 128);

            row0 += 8;
            row1 += 8;
            y0 += 2;
            y1 += 2;
            uv += 2;
        }
    }
}

bool Encoder::submitDxgiSurface(ID3D11Texture2D* tex, uint64_t ptsUs,
                                std::vector<uint8_t>& outAnnexB, bool& outIsKeyframe, bool& outInputAccepted) {
    outInputAccepted = false;
    if (!tex) return false;

    IMFMediaBuffer* mfBuf = nullptr;
    if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), tex, 0, FALSE, &mfBuf)) || !mfBuf) {
        return false;
    }

    IMF2DBuffer* buf2d = nullptr;
    if (SUCCEEDED(mfBuf->QueryInterface(__uuidof(IMF2DBuffer), reinterpret_cast<void**>(&buf2d))) && buf2d) {
        DWORD contigLen = 0;
        if (SUCCEEDED(buf2d->GetContiguousLength(&contigLen))) {
            mfBuf->SetCurrentLength(contigLen);
        }
        buf2d->Release();
    }

    IMFSample* sample = nullptr;
    if (FAILED(MFCreateSample(&sample)) || !sample) {
        mfBuf->Release();
        return false;
    }
    sample->AddBuffer(mfBuf);
    mfBuf->Release();
    sample->SetSampleTime(static_cast<LONGLONG>(ptsUs) * 10);
    sample->SetSampleDuration(10000000LL / m_fps);
    return submitSampleToMft(sample, outAnnexB, outIsKeyframe, outInputAccepted);
}

bool Encoder::encodeGpuFrame(ID3D11Texture2D* gpuSrcTex, uint64_t ptsUs,
                             std::vector<uint8_t>& outAnnexB, bool& outIsKeyframe) {
    if (!m_mft || !m_hasVideoProcessor || !gpuSrcTex || !m_videoDevice || !m_videoContext) return false;

    int slot = m_vpRingIdx;
    m_vpRingIdx = (m_vpRingIdx + 1) % kVpRingSize;
    ID3D11Texture2D* outTex = m_vpOutTex[slot];
    ID3D11VideoProcessorOutputView* outView = m_vpOutView[slot];
    if (!outTex || !outView) return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc = {};
    ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivDesc.Texture2D.MipSlice = 0;
    ivDesc.Texture2D.ArraySlice = 0;

    ID3D11VideoProcessorInputView* inView = nullptr;
    HRESULT hr = m_videoDevice->CreateVideoProcessorInputView(gpuSrcTex, m_vpEnum, &ivDesc, &inView);
    if (FAILED(hr) || !inView) return false;

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = inView;

    hr = m_videoContext->VideoProcessorBlt(m_vp, outView, 0, 1, &stream);
    inView->Release();
    if (FAILED(hr)) return false;

    // Flush VideoProcessorBlt to GPU queue immediately so NVENC reads current frame without batch stall
    m_d3dContext->Flush();

    if (m_useD3dManager) {
        if (!waitAsyncInputReady()) return popPendingFrame(outAnnexB, outIsKeyframe);
        ID3D11Texture2D* submitTex = outTex;
        if (m_nvDevice) {
            if (!m_nvGpuTex[slot] || !copyStagedYuvToNvTexture(outTex, m_nvGpuTex[slot])) {
                return popPendingFrame(outAnnexB, outIsKeyframe);
            }
            submitTex = m_nvGpuTex[slot];
        }

        bool inputAccepted = false;
        bool gotOut = submitDxgiSurface(submitTex, ptsUs, outAnnexB, outIsKeyframe, inputAccepted);
        if (inputAccepted) {
            return gotOut;
        }
        // Only disable DXGI manager if ProcessInput actually rejected the D3D11 surface
        m_useD3dManager = false;
        m_mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
    }

    if (!copyTextureToPackedYuv(outTex, m_yuvBuffer)) return false;
    return encodePackedYuv(m_yuvBuffer, ptsUs, outAnnexB, outIsKeyframe);
}

bool Encoder::encodeBgraCpu(const uint8_t* bgra, uint32_t width, uint32_t height, uint64_t ptsUs,
                            std::vector<uint8_t>& outAnnexB, bool& outIsKeyframe) {
    if (!m_mft || !bgra || width != m_width || height != m_height || m_useP010) return false;
    convertBgraToNv12Studio(bgra, width, height, m_yuvBuffer);
    return encodePackedYuv(m_yuvBuffer, ptsUs, outAnnexB, outIsKeyframe);
}

bool Encoder::popPendingFrame(std::vector<uint8_t>& outAnnexB, bool& outIsKeyframe) {
    if (m_readyOutputs.empty()) {
        outAnnexB.clear();
        outIsKeyframe = false;
        return false;
    }
    outAnnexB = std::move(m_readyOutputs.front().first);
    outIsKeyframe = m_readyOutputs.front().second;
    m_readyOutputs.pop_front();
    return !outAnnexB.empty();
}

bool Encoder::waitAsyncInputReady() {
    if (!m_isAsync) return true;

    pumpAsyncEvents(false);

    auto waitStart = std::chrono::steady_clock::now();
    while (m_needInputCount <= 0) {
        pumpAsyncEvents(false);
        if (m_needInputCount > 0) break;
        auto elapsed = std::chrono::steady_clock::now() - waitStart;
        if (elapsed > std::chrono::milliseconds(20)) {
            return false;
        }
        if (elapsed < std::chrono::microseconds(500)) {
            Sleep(0);
        } else {
            Sleep(1);
        }
    }
    return true;
}

bool Encoder::submitSampleToMft(IMFSample* sample, std::vector<uint8_t>& outAnnexB, bool& outIsKeyframe, bool& outInputAccepted) {
    outInputAccepted = false;
    if (!sample) return false;

    if (m_forceKeyframe.exchange(false) && m_codecApi) {
        setCodecUint32(m_codecApi, CODECAPI_AVEncVideoForceKeyFrame, 1);
    }

    HRESULT hr = m_mft->ProcessInput(m_inStreamId, sample, 0);
    sample->Release();

    if (FAILED(hr)) {
        return popPendingFrame(outAnnexB, outIsKeyframe);
    }

    outInputAccepted = true;
    if (m_isAsync) {
        m_needInputCount--;
        pumpAsyncEvents(true);
        return popPendingFrame(outAnnexB, outIsKeyframe);
    }
    return collectSyncOutput(outAnnexB, outIsKeyframe);
}

bool Encoder::encodePackedYuv(const std::vector<uint8_t>& yuvData, uint64_t ptsUs,
                              std::vector<uint8_t>& outAnnexB, bool& outIsKeyframe) {
    if (!m_mft || yuvData.empty()) return false;
    if (!waitAsyncInputReady()) return false;

    IMFMediaBuffer* mfBuf = nullptr;
    DWORD dataSize = static_cast<DWORD>(yuvData.size());
    if (FAILED(MFCreateMemoryBuffer(dataSize, &mfBuf))) return false;

    BYTE* dst = nullptr;
    if (SUCCEEDED(mfBuf->Lock(&dst, nullptr, nullptr))) {
        memcpy(dst, yuvData.data(), dataSize);
        mfBuf->Unlock();
        mfBuf->SetCurrentLength(dataSize);
    }

    IMFSample* sample = nullptr;
    if (FAILED(MFCreateSample(&sample))) {
        mfBuf->Release();
        return false;
    }
    sample->AddBuffer(mfBuf);
    mfBuf->Release();
    sample->SetSampleTime(static_cast<LONGLONG>(ptsUs) * 10);
    sample->SetSampleDuration(10000000LL / m_fps);

    bool inputAccepted = false;
    return submitSampleToMft(sample, outAnnexB, outIsKeyframe, inputAccepted);
}

bool Encoder::collectSyncOutput(std::vector<uint8_t>& outAnnexB, bool& outIsKeyframe) {
    outAnnexB.clear();
    outIsKeyframe = false;

    for (;;) {
        MFT_OUTPUT_STREAM_INFO info = {};
        m_mft->GetOutputStreamInfo(m_outStreamId, &info);

        MFT_OUTPUT_DATA_BUFFER outBuf = {};
        outBuf.dwStreamID = m_outStreamId;
        IMFSample* outSample = nullptr;

        if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            if (FAILED(MFCreateSample(&outSample))) break;
            IMFMediaBuffer* buf = nullptr;
            if (FAILED(MFCreateMemoryBuffer(m_outBufSize, &buf))) {
                outSample->Release();
                break;
            }
            outSample->AddBuffer(buf);
            buf->Release();
            outBuf.pSample = outSample;
        }

        DWORD status = 0;
        HRESULT hr = m_mft->ProcessOutput(0, 1, &outBuf, &status);

        if (outBuf.pEvents) outBuf.pEvents->Release();

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (outBuf.pSample) outBuf.pSample->Release();
            IMFMediaType* newOutType = nullptr;
            if (SUCCEEDED(m_mft->GetOutputAvailableType(m_outStreamId, 0, &newOutType)) && newOutType) {
                m_mft->SetOutputType(m_outStreamId, newOutType, 0);
                UINT32 hdrLen = 0;
                if (SUCCEEDED(newOutType->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &hdrLen)) && hdrLen > 0) {
                    m_seqHeader.resize(hdrLen);
                    newOutType->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, m_seqHeader.data(), hdrLen, &hdrLen);
                }
                newOutType->Release();
            }
            continue;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT || FAILED(hr)) {
            if (outBuf.pSample) outBuf.pSample->Release();
            break;
        }

        if (outBuf.pSample) {
            UINT32 cleanPoint = 0;
            if (SUCCEEDED(outBuf.pSample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint)) && cleanPoint) {
                outIsKeyframe = true;
            }

            IMFMediaBuffer* contiguous = nullptr;
            if (SUCCEEDED(outBuf.pSample->ConvertToContiguousBuffer(&contiguous)) && contiguous) {
                BYTE* pBytes = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(contiguous->Lock(&pBytes, nullptr, &len)) && len > 0) {
                    if (outIsKeyframe && !m_seqHeader.empty()) {
                        // Ensure SPS/PPS(/VPS) precede keyframe if driver omitted inline headers
                        bool hasStartCode = (len > 4 && pBytes[0] == 0 && pBytes[1] == 0);
                        uint8_t firstNalType = 0;
                        if (hasStartCode) {
                            size_t off = (pBytes[2] == 1) ? 3 : 4;
                            if (off < len) {
                                firstNalType = (m_codec == CODEC_HEVC) ? ((pBytes[off] >> 1) & 0x3F) : (pBytes[off] & 0x1F);
                            }
                        }
                        bool alreadyHasParamSets = (m_codec == CODEC_HEVC) ? (firstNalType == 32 || firstNalType == 33) : (firstNalType == 7);
                        if (!alreadyHasParamSets) {
                            outAnnexB.insert(outAnnexB.end(), m_seqHeader.begin(), m_seqHeader.end());
                        }
                    }
                    outAnnexB.insert(outAnnexB.end(), pBytes, pBytes + len);
                    contiguous->Unlock();
                }
                contiguous->Release();
            }
            outBuf.pSample->Release();
        }

        if (!outAnnexB.empty()) return true;
    }

    return !outAnnexB.empty();
}

bool Encoder::pumpAsyncEvents(bool waitOutput) {
    if (!m_eventGen) return false;

    auto start = std::chrono::steady_clock::now();

    for (;;) {
        IMFMediaEvent* ev = nullptr;
        HRESULT hr = m_eventGen->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) {
            if (!waitOutput || !m_readyOutputs.empty()) break;
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::milliseconds(20)) break;
            if (elapsed < std::chrono::microseconds(500)) {
                Sleep(0);
            } else {
                Sleep(1);
            }
            continue;
        }
        if (FAILED(hr) || !ev) break;

        MediaEventType met = MEUnknown;
        ev->GetType(&met);
        ev->Release();

        if (met == METransformNeedInput) {
            m_needInputCount++;
        } else if (met == METransformHaveOutput) {
            std::vector<uint8_t> pkt;
            bool isKey = false;
            if (collectSyncOutput(pkt, isKey) && !pkt.empty()) {
                m_readyOutputs.emplace_back(std::move(pkt), isKey);
            }
        }
    }

    return !m_readyOutputs.empty();
}
