#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

struct GlassColor {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;
};

namespace glass {

inline uint32_t packPremul(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint32_t pr = (static_cast<uint32_t>(r) * a + 127) / 255;
    uint32_t pg = (static_cast<uint32_t>(g) * a + 127) / 255;
    uint32_t pb = (static_cast<uint32_t>(b) * a + 127) / 255;
    return (static_cast<uint32_t>(a) << 24) | (pr << 16) | (pg << 8) | pb;
}

inline void blendPremul(uint32_t& dst, uint32_t sr, uint32_t sg, uint32_t sb, uint32_t sa) {
    if (sa == 0) return;
    if (sa >= 255) {
        dst = (255u << 24) | (sr << 16) | (sg << 8) | sb;
        return;
    }
    uint32_t inv = 255u - sa;
    uint32_t da = (dst >> 24) & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >> 8) & 0xFF;
    uint32_t db = dst & 0xFF;

    uint32_t oa = std::min<uint32_t>(255u, sa + (da * inv + 127) / 255);
    uint32_t orr = std::min<uint32_t>(oa, sr + (dr * inv + 127) / 255);
    uint32_t og = std::min<uint32_t>(oa, sg + (dg * inv + 127) / 255);
    uint32_t ob = std::min<uint32_t>(oa, sb + (db * inv + 127) / 255);
    dst = (oa << 24) | (orr << 16) | (og << 8) | ob;
}

inline void blendStraight(uint32_t& dst, GlassColor col, float cov) {
    if (cov <= 0.0f || col.a == 0) return;
    uint32_t sa = static_cast<uint32_t>(std::lround(col.a * std::min(1.0f, cov)));
    if (sa == 0) return;
    uint32_t sr = (static_cast<uint32_t>(col.r) * sa + 127) / 255;
    uint32_t sg = (static_cast<uint32_t>(col.g) * sa + 127) / 255;
    uint32_t sb = (static_cast<uint32_t>(col.b) * sa + 127) / 255;
    blendPremul(dst, sr, sg, sb, sa);
}

inline void fillCanvasGlass(uint32_t* px, int w, int h, uint8_t baseAlpha) {
    if (!px || w <= 0 || h <= 0) return;
    const float fw = static_cast<float>(std::max(1, w));
    const float fh = static_cast<float>(std::max(1, h));
    for (int y = 0; y < h; ++y) {
        float ty = y / fh;
        float dy = ty - 0.08f;
        for (int x = 0; x < w; ++x) {
            float tx = x / fw;
            float dx = tx - 0.14f;
            // Pitch-black obsidian base with a restrained top-left crimson studio ambient bloom
            float d2 = dx * dx + dy * dy * 1.4f;
            float glow = std::exp(-d2 * 4.8f);
            int r = static_cast<int>(9 + 28 * glow + 3 * tx);
            int g = static_cast<int>(9 + 3 * glow + 2 * ty);
            int b = static_cast<int>(12 + 5 * glow + 4 * ty);
            px[static_cast<size_t>(y) * w + x] = packPremul(
                static_cast<uint8_t>(std::min(255, r)),
                static_cast<uint8_t>(std::min(255, g)),
                static_cast<uint8_t>(std::min(255, b)),
                baseAlpha);
        }
    }
}

inline void fillRoundRectGrad(uint32_t* px, int w, int h,
                              float x0, float y0, float x1, float y1, float rad,
                              GlassColor cStart, GlassColor cEnd,
                              GlassColor border, float borderPx = 1.0f) {
    if (!px || x1 <= x0 || y1 <= y0) return;
    const int ix0 = std::max(0, static_cast<int>(std::floor(x0 - 2.0f)));
    const int iy0 = std::max(0, static_cast<int>(std::floor(y0 - 2.0f)));
    const int ix1 = std::min(w, static_cast<int>(std::ceil(x1 + 2.0f)));
    const int iy1 = std::min(h, static_cast<int>(std::ceil(y1 + 2.0f)));

    const float cx = (x0 + x1) * 0.5f;
    const float cy = (y0 + y1) * 0.5f;
    const float rw = (x1 - x0) * 0.5f;
    const float rh = (y1 - y0) * 0.5f;
    const float rClamped = std::min(rad, std::min(rw, rh));
    const float hw = rw - rClamped;
    const float hh = rh - rClamped;
    const float invW = 1.0f / std::max(1.0f, x1 - x0);
    const float invH = 1.0f / std::max(1.0f, y1 - y0);

    for (int y = iy0; y < iy1; ++y) {
        const float ty = std::clamp((y + 0.5f - y0) * invH, 0.0f, 1.0f);
        const float ay = std::fabs((y + 0.5f) - cy) - hh;
        uint32_t* row = px + static_cast<size_t>(y) * w;
        for (int x = ix0; x < ix1; ++x) {
            const float ax = std::fabs((x + 0.5f) - cx) - hw;
            float dist = (ax > 0.0f && ay > 0.0f)
                             ? (std::sqrt(ax * ax + ay * ay) - rClamped)
                             : (std::max(ax, ay) - rClamped);

            float fillCov = std::clamp(0.5f - dist, 0.0f, 1.0f);
            if (fillCov > 0.0f) {
                float tx = std::clamp((x + 0.5f - x0) * invW, 0.0f, 1.0f);
                float t = 0.72f * tx + 0.28f * ty;
                GlassColor fc = {
                    static_cast<uint8_t>(std::lround(cStart.r + (cEnd.r - cStart.r) * t)),
                    static_cast<uint8_t>(std::lround(cStart.g + (cEnd.g - cStart.g) * t)),
                    static_cast<uint8_t>(std::lround(cStart.b + (cEnd.b - cStart.b) * t)),
                    static_cast<uint8_t>(std::lround(cStart.a + (cEnd.a - cStart.a) * t))
                };
                blendStraight(row[x], fc, fillCov);
            }
            if (border.a > 0) {
                float ringHalf = std::max(0.5f, borderPx * 0.5f);
                float bdCov = std::clamp(ringHalf + 0.5f - std::fabs(dist + ringHalf), 0.0f, 1.0f);
                if (bdCov > 0.0f) {
                    blendStraight(row[x], border, bdCov);
                }
            }
        }
    }
}

inline void strokeRoundRect(uint32_t* px, int w, int h,
                            float x0, float y0, float x1, float y1, float rad,
                            float strokePx, GlassColor col) {
    fillRoundRectGrad(px, w, h, x0, y0, x1, y1, rad, {0, 0, 0, 0}, {0, 0, 0, 0}, col, strokePx);
}

inline void drawAaSegment(uint32_t* px, int w, int h,
                          float x0, float y0, float x1, float y1,
                          float stroke, GlassColor col) {
    if (!px || col.a == 0) return;
    const float halfS = stroke * 0.5f + 0.5f;
    const int ix0 = std::max(0, static_cast<int>(std::floor(std::min(x0, x1) - halfS - 1.0f)));
    const int iy0 = std::max(0, static_cast<int>(std::floor(std::min(y0, y1) - halfS - 1.0f)));
    const int ix1 = std::min(w, static_cast<int>(std::ceil(std::max(x0, x1) + halfS + 1.0f)));
    const int iy1 = std::min(h, static_cast<int>(std::ceil(std::max(y0, y1) + halfS + 1.0f)));

    const float vx = x1 - x0;
    const float vy = y1 - y0;
    const float vv = vx * vx + vy * vy;

    for (int y = iy0; y < iy1; ++y) {
        uint32_t* row = px + static_cast<size_t>(y) * w;
        for (int x = ix0; x < ix1; ++x) {
            float dx = (x + 0.5f) - x0;
            float dy = (y + 0.5f) - y0;
            float t = (vv > 1e-5f) ? std::clamp((dx * vx + dy * vy) / vv, 0.0f, 1.0f) : 0.0f;
            float pxD = dx - t * vx;
            float pyD = dy - t * vy;
            float dist = std::sqrt(pxD * pxD + pyD * pyD);
            float cov = std::clamp(halfS - dist, 0.0f, 1.0f);
            if (cov > 0.0f) blendStraight(row[x], col, cov);
        }
    }
}

inline void fillAaCircle(uint32_t* px, int w, int h, float cx, float cy, float r, GlassColor col) {
    if (!px || col.a == 0 || r <= 0.0f) return;
    const int ix0 = std::max(0, static_cast<int>(std::floor(cx - r - 1.5f)));
    const int iy0 = std::max(0, static_cast<int>(std::floor(cy - r - 1.5f)));
    const int ix1 = std::min(w, static_cast<int>(std::ceil(cx + r + 1.5f)));
    const int iy1 = std::min(h, static_cast<int>(std::ceil(cy + r + 1.5f)));
    for (int y = iy0; y < iy1; ++y) {
        uint32_t* row = px + static_cast<size_t>(y) * w;
        for (int x = ix0; x < ix1; ++x) {
            float dist = std::hypot((x + 0.5f) - cx, (y + 0.5f) - cy) - r;
            float cov = std::clamp(0.5f - dist, 0.0f, 1.0f);
            if (cov > 0.0f) blendStraight(row[x], col, cov);
        }
    }
}

inline void drawAaCircleRing(uint32_t* px, int w, int h, float cx, float cy, float r, float stroke, GlassColor col) {
    if (!px || col.a == 0 || r <= 0.0f) return;
    const float halfS = stroke * 0.5f + 0.5f;
    const int ix0 = std::max(0, static_cast<int>(std::floor(cx - r - halfS - 1.5f)));
    const int iy0 = std::max(0, static_cast<int>(std::floor(cy - r - halfS - 1.5f)));
    const int ix1 = std::min(w, static_cast<int>(std::ceil(cx + r + halfS + 1.5f)));
    const int iy1 = std::min(h, static_cast<int>(std::ceil(cy + r + halfS + 1.5f)));
    for (int y = iy0; y < iy1; ++y) {
        uint32_t* row = px + static_cast<size_t>(y) * w;
        for (int x = ix0; x < ix1; ++x) {
            float dist = std::fabs(std::hypot((x + 0.5f) - cx, (y + 0.5f) - cy) - r);
            float cov = std::clamp(halfS - dist, 0.0f, 1.0f);
            if (cov > 0.0f) blendStraight(row[x], col, cov);
        }
    }
}

inline void fillAaPlayTriangle(uint32_t* px, int w, int h, float cx, float cy, float size, GlassColor col) {
    if (!px || col.a == 0) return;
    float x0 = cx - size * 0.45f, y0 = cy - size * 0.55f;
    float x1 = cx + size * 0.55f, y1 = cy;
    float x2 = cx - size * 0.45f, y2 = cy + size * 0.55f;
    auto edge = [](float ax, float ay, float bx, float by, float pxf, float pyf) {
        return (pxf - ax) * (by - ay) - (pyf - ay) * (bx - ax);
    };
    const float len0 = std::hypot(x1 - x0, y1 - y0);
    const float len1 = std::hypot(x2 - x1, y2 - y1);
    const float len2 = std::hypot(x0 - x2, y0 - y2);
    // Flip sign so points inside all three half-spaces always produce negative SDF distance
    const float sign = (edge(x0, y0, x1, y1, cx, cy) > 0.0f) ? -1.0f : 1.0f;

    const int ix0 = std::max(0, static_cast<int>(std::floor(x0 - 1.5f)));
    const int iy0 = std::max(0, static_cast<int>(std::floor(y0 - 1.5f)));
    const int ix1 = std::min(w, static_cast<int>(std::ceil(x1 + 1.5f)));
    const int iy1 = std::min(h, static_cast<int>(std::ceil(y2 + 1.5f)));
    for (int y = iy0; y < iy1; ++y) {
        uint32_t* row = px + static_cast<size_t>(y) * w;
        for (int x = ix0; x < ix1; ++x) {
            float fx = x + 0.5f, fy = y + 0.5f;
            float d0 = edge(x0, y0, x1, y1, fx, fy) / len0;
            float d1 = edge(x1, y1, x2, y2, fx, fy) / len1;
            float d2 = edge(x2, y2, x0, y0, fx, fy) / len2;
            float dist = std::max({d0 * sign, d1 * sign, d2 * sign});
            float cov = std::clamp(0.5f - dist, 0.0f, 1.0f);
            if (cov > 0.0f) blendStraight(row[x], col, cov);
        }
    }
}

} // namespace glass
