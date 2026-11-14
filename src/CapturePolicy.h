#pragma once

#include <cstdint>

inline bool shouldAcceptDxgiFrame(uint64_t lastPresentTime, uint32_t accumulatedFrames,
                                  bool hasValidGpuFrame, bool requireLivePresent) {
    if (lastPresentTime != 0 || accumulatedFrames > 0) return true;
    return !requireLivePresent && !hasValidGpuFrame;
}

inline bool cooldownElapsedMs(uint64_t nowMs, uint64_t lastAttemptMs, uint64_t cooldownMs) {
    if (lastAttemptMs == 0) return true;
    return nowMs >= lastAttemptMs && (nowMs - lastAttemptMs) >= cooldownMs;
}

inline bool shouldStepDownToGdi(uint32_t gpuReinitFails, uint32_t maxFails = 5) {
    return gpuReinitFails >= maxFails;
}
