// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

/**
 * Real-time peak metering helpers for the audio thread.
 *
 * Sample-peak (block maximum absolute sample) per channel — appropriate for per-track strips
 * (instantaneous level, minimal CPU). True-peak / oversampled metering belongs on the master
 * or dedicated analysis paths, not here.
 *
 * Threading: TrackMeterBlockAccum is written only on the audio thread; atomics in LooperEngine
 * are published once per block with memory_order_relaxed (UI reads on timer/message thread).
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace retrolooper::metering
{
/** Keep equal to LooperEngine::kMaxTracks. */
inline constexpr std::size_t kMeterTracks = 8;

struct TrackMeterBlockAccum final
{
    std::array<float, kMeterTracks> outPeakL{};
    std::array<float, kMeterTracks> outPeakR{};
    std::array<float, kMeterTracks> inPeakL{};
    std::array<float, kMeterTracks> inPeakR{};
    /** Sum of squares for block RMS (per channel). */
    std::array<float, kMeterTracks> outSumSqL{};
    std::array<float, kMeterTracks> outSumSqR{};
    std::array<float, kMeterTracks> inSumSqL{};
    std::array<float, kMeterTracks> inSumSqR{};
    /** Stereo playback samples counted toward out RMS (one increment per L/R pair). */
    std::array<int, kMeterTracks> outSampleCount{};
    /** Mono/stereo input samples per channel for RMS. */
    std::array<int, kMeterTracks> inSampleCountL{};
    std::array<int, kMeterTracks> inSampleCountR{};
};

[[nodiscard]] inline float blockRms(const float sumSq, const int count) noexcept
{
    if (count <= 0 || !(sumSq > 0.f))
    {
        return 0.f;
    }
    return std::sqrt(sumSq / static_cast<float>(count));
}

[[nodiscard]] inline bool sampleClipped(const float x) noexcept
{
    return std::abs(x) >= 1.0f;
}

inline void accumulateOutputStereo(TrackMeterBlockAccum& m,
    const std::size_t trackIndex,
    const float l,
    const float r) noexcept
{
    const float al = std::abs(l);
    const float ar = std::abs(r);
    float& pkL = m.outPeakL[trackIndex];
    float& pkR = m.outPeakR[trackIndex];
    pkL = (al > pkL) ? al : pkL;
    pkR = (ar > pkR) ? ar : pkR;
    m.outSumSqL[trackIndex] += l * l;
    m.outSumSqR[trackIndex] += r * r;
    ++m.outSampleCount[trackIndex];
}

inline void accumulateInputChannel(TrackMeterBlockAccum& m,
    const std::size_t trackIndex,
    const int channel,
    const float s) noexcept
{
    const float a = std::abs(s);
    if (channel <= 0)
    {
        float& pk = m.inPeakL[trackIndex];
        pk = (a > pk) ? a : pk;
        m.inSumSqL[trackIndex] += s * s;
        ++m.inSampleCountL[trackIndex];
    }
    else
    {
        float& pk = m.inPeakR[trackIndex];
        pk = (a > pk) ? a : pk;
        m.inSumSqR[trackIndex] += s * s;
        ++m.inSampleCountR[trackIndex];
    }
}

/** Snapshot of linear levels read from atomics (0…1 typical; may exceed 1 when hot). */
struct TrackMeterRtSnapshot final
{
    float outPeakL = 0.f;
    float outPeakR = 0.f;
    float inPeakL = 0.f;
    float inPeakR = 0.f;
    float outRmsL = 0.f;
    float outRmsR = 0.f;
    float inRmsL = 0.f;
    float inRmsR = 0.f;
};

} // namespace retrolooper::metering
