// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "EngineTypes.h"

#include <cstddef>
#include <vector>

namespace retrolooper
{
class Track;

class AudioImportAnalysisService
{
  public:
    struct Config
    {
        double minBpm = 40.0;
        double maxBpm = 240.0;
        double maxAnalysisSeconds = 45.0;
        double silenceThresholdLinear = 0.015;
    };

    static TempoDetectionResult analyzeTrack(const Track& track,
        double sampleRate,
        double projectTempoBpm,
        int globalBarLengthBars,
        double quartersPerNotatedBar,
        const Config& cfg) noexcept;

    static TempoDetectionResult analyzeTrack(const Track& track,
        double sampleRate,
        double projectTempoBpm,
        int globalBarLengthBars,
        double quartersPerNotatedBar) noexcept
    {
        return analyzeTrack(track, sampleRate, projectTempoBpm, globalBarLengthBars, quartersPerNotatedBar, Config{});
    }

  private:
    static std::vector<float> buildMono(const Track& track, std::size_t maxSamples) noexcept;
};
} // namespace retrolooper
