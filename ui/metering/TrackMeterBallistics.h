// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "../../engine/EngineTypes.h"
#include "../../engine/Track.h"
#include "../../engine/metering/PeakMeter.h"

#include <algorithm>
#include <cmath>

namespace retrolooper::meter_ui
{
/**
 * Message-thread ballistics for loop-track meters: instant attack, exponential release,
 * slower peak-hold decay, optional RMS smoothing, clip latch timer.
 *
 * Input and output paths are tracked independently so the track strip can show both VUs.
 * Feed on the UI animation tick with measured dt, independently of control synchronization.
 */
class TrackMeterBallistics final
{
  public:
    void reset() noexcept
    {
        out_ = {};
        in_ = {};
        clipLatched_ = false;
        clipHoldRemaining_ = 0.f;
        inputPath_ = false;
    }

    void process(const float dtSec,
        const metering::TrackMeterRtSnapshot& snap,
        const TrackState trackState) noexcept
    {
        inputPath_ = (trackState == TrackState::Recording || trackState == TrackState::Armed
            || trackState == TrackState::Empty);
        updatePath(out_, dtSec, snap.outPeakL, snap.outPeakR, snap.outRmsL, snap.outRmsR);
        updatePath(in_, dtSec, snap.inPeakL, snap.inPeakR, snap.inRmsL, snap.inRmsR);

        const float rawMaxLin = std::max(
            std::max(snap.outPeakL, snap.outPeakR), std::max(snap.inPeakL, snap.inPeakR));
        if (rawMaxLin >= 1.0f)
        {
            clipLatched_ = true;
            clipHoldRemaining_ = clipLatchHoldSec_;
        }
        else if (clipHoldRemaining_ > 0.f)
        {
            clipHoldRemaining_ -= dtSec;
        }
        else
        {
            clipLatched_ = false;
        }
    }

    /** Visualizer / combined focus meter: input while empty, armed, or recording; playback otherwise. */
    [[nodiscard]] float displayLevel() const noexcept
    {
        return focus().level;
    }
    [[nodiscard]] float displayLevelL() const noexcept
    {
        return focus().l;
    }
    [[nodiscard]] float displayLevelR() const noexcept
    {
        return focus().r;
    }
    [[nodiscard]] float peakHold() const noexcept
    {
        return focus().hold;
    }
    [[nodiscard]] float peakHoldL() const noexcept
    {
        return focus().holdL;
    }
    [[nodiscard]] float peakHoldR() const noexcept
    {
        return focus().holdR;
    }
    [[nodiscard]] float rmsDisplay() const noexcept
    {
        return focus().rms;
    }
    [[nodiscard]] float rmsDisplayL() const noexcept
    {
        return focus().rmsL;
    }
    [[nodiscard]] float rmsDisplayR() const noexcept
    {
        return focus().rmsR;
    }
    [[nodiscard]] float displayOutputLevel() const noexcept
    {
        return out_.level;
    }
    [[nodiscard]] float displayInputLevel() const noexcept
    {
        return in_.level;
    }
    [[nodiscard]] bool clipLatched() const noexcept
    {
        return clipLatched_;
    }
    [[nodiscard]] bool isInputPath() const noexcept
    {
        return inputPath_;
    }

  private:
    struct Path
    {
        float level{0.f};
        float l{0.f};
        float r{0.f};
        float hold{0.f};
        float holdL{0.f};
        float holdR{0.f};
        float rms{0.f};
        float rmsL{0.f};
        float rmsR{0.f};
    };

    static constexpr float levelFallTauSec_ = 0.30f;
    static constexpr float peakHoldFallTauSec_ = 1.15f;
    static constexpr float rmsSmoothTauSec_ = 0.40f;
    static constexpr float clipLatchHoldSec_ = 2.2f;

    [[nodiscard]] static float linToMeter(const float lin) noexcept
    {
        constexpr float floorDb = -48.0f;
        if (!(lin > 1.0e-6f))
        {
            return 0.0f;
        }
        const float db = 20.0f * std::log10(lin);
        return std::clamp((db - floorDb) / -floorDb, 0.0f, 1.0f);
    }

    static void updatePath(Path& p, const float dtSec, const float pkLinL, const float pkLinR,
        const float rmsLinL, const float rmsLinR) noexcept
    {
        const float pkL = linToMeter(pkLinL);
        const float pkR = linToMeter(pkLinR);
        const float rawMax = std::max(pkL, pkR);
        const float rmsL = linToMeter(rmsLinL);
        const float rmsR = linToMeter(rmsLinR);
        const float rmsMax = std::max(rmsL, rmsR);

        auto fallToward = [dtSec](float& current, const float raw, const float tau) noexcept
        {
            if (raw >= current)
            {
                current = raw;
            }
            else
            {
                current = raw + (current - raw) * std::exp(-dtSec / tau);
            }
        };

        fallToward(p.level, rawMax, levelFallTauSec_);
        fallToward(p.l, pkL, levelFallTauSec_);
        fallToward(p.r, pkR, levelFallTauSec_);
        fallToward(p.hold, rawMax, peakHoldFallTauSec_);
        fallToward(p.holdL, pkL, peakHoldFallTauSec_);
        fallToward(p.holdR, pkR, peakHoldFallTauSec_);
        p.hold = std::max(p.hold, p.level);
        p.holdL = std::max(p.holdL, p.l);
        p.holdR = std::max(p.holdR, p.r);

        const float rmsA = 1.f - std::exp(-dtSec / rmsSmoothTauSec_);
        p.rms += (rmsMax - p.rms) * rmsA;
        p.rmsL += (rmsL - p.rmsL) * rmsA;
        p.rmsR += (rmsR - p.rmsR) * rmsA;
    }

    [[nodiscard]] const Path& focus() const noexcept
    {
        return inputPath_ ? in_ : out_;
    }

    Path out_{};
    Path in_{};
    float clipHoldRemaining_{0.f};
    bool clipLatched_{false};
    bool inputPath_{false};
};

} // namespace retrolooper::meter_ui
