#pragma once

#include <cstdint>

inline bool shouldAcceptDxgiFrame(uint64_t lastPresentTime, uint32_t accumulatedFrames,
                                  bool /*hasValidGpuFrame*/, bool /*requireLivePresent*/) {
    // Reject uninitialized zero/zero frames even on calm desktops so black DXGI buffers never stick
    return lastPresentTime != 0 || accumulatedFrames > 0;
}

inline bool cooldownElapsedMs(uint64_t nowMs, uint64_t lastAttemptMs, uint64_t cooldownMs) {
    if (lastAttemptMs == 0) return true;
    return nowMs >= lastAttemptMs && (nowMs - lastAttemptMs) >= cooldownMs;
}

inline bool shouldStepDownToGdi(uint32_t gpuReinitFails, uint32_t maxFails = 5) {
    return gpuReinitFails >= maxFails;
}

inline uint32_t resolveTargetFps(int cliFps, uint32_t monitorHz, bool& outUnlimited) {
    if (cliFps < 0) {
        outUnlimited = true;
        return (monitorHz >= 30u && monitorHz <= 1000u) ? monitorHz : 240u;
    }
    outUnlimited = false;
    if (cliFps == 0) {
        return (monitorHz >= 30u && monitorHz <= 480u) ? monitorHz : 60u;
    }
    if (cliFps < 15) return 15u;
    if (cliFps > 1000) return 1000u;
    return static_cast<uint32_t>(cliFps);
}

inline uint32_t computeBitrateMbps(uint32_t width, uint32_t height, uint32_t fps, bool p010, bool hevc) {
    bool highRes = ((static_cast<uint64_t>(width) * height) > 3000000ULL);
    uint32_t base = p010 ? (highRes ? 90u : 52u) : (hevc ? (highRes ? 72u : 42u) : (highRes ? 84u : 48u));
    uint32_t clampedFps = fps < 60u ? 60u : (fps > 240u ? 240u : fps);
    uint32_t scaled = base + (base * (clampedFps - 60u)) / 120u;
    return scaled > 200u ? 200u : scaled;
}

inline int stepBitrateScalePct(int currentPct, bool congested) {
    if (congested) {
        int next = currentPct - 15;
        return next < 20 ? 20 : next;
    }
    int next = currentPct + 10;
    return next > 100 ? 100 : next;
}
