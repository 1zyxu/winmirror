#pragma once

#include "Common.h"
#include <wincodec.h>
#include <vector>

class Encoder {
public:
    Encoder();
    ~Encoder();

    bool init(float quality = 0.75f);
    void cleanup();

    bool encodeBgraToJpeg(const uint8_t* bgra, uint32_t width, uint32_t height, std::vector<uint8_t>& outJpeg);

    void setQuality(float q) { m_quality = (q < 0.1f) ? 0.1f : (q > 1.0f ? 1.0f : q); }
    float getQuality() const { return m_quality; }

private:
    float m_quality = 0.75f;
    IWICImagingFactory* m_factory = nullptr;
    bool m_coInitialized = false;
};
