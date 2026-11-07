// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>

namespace retrolooper
{
/** BPM window searched by {@code estimateTempoFromMonoAudio} (import / audio-based analysis). */
inline constexpr double kTempoDetectionBpmMin = 40.0;
inline constexpr double kTempoDetectionBpmMax = 240.0;

/**
 * Tempo hinting from multi-band onset flux + constrained BPM hypothesis scoring
 * (autocorr lag search with half/double disambiguation and confidence).
 */
struct TempoEstimateResult
{
    /** Best-effort BPM when confidence > 0; otherwise undefined. */
    double bpm = 0.0;
    /** 0 = no usable estimate, 1 = strong peak vs neighbours. */
    double confidence = 0.0;
    /** Detected beat period in samples (0 when unknown). */
    double beatPeriodSamples = 0.0;
    /** Estimated beat phase origin relative to sample 0, in samples [0, beatPeriod). */
    double phaseOffsetSamples = 0.0;
};

TempoEstimateResult estimateTempoFromMonoAudio(const float* mono,
    std::size_t numSamples,
    double sampleRate,
    double minBpm,
    double maxBpm,
    int countsPerBar = 4) noexcept;
} // namespace retrolooper
