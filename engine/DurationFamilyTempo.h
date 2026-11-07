// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cmath>

namespace retrolooper
{
/**
 * Source BPM from loop duration using the 1/2/4/8/16-bar family (4/4),
 * octave-folded into a playable band and scored against {@code sessionHintBpm}.
 * Same math as Sound Crate sync preview, so a follower import stretches at the
 * same ratio the audition used.
 */
[[nodiscard]] inline double estimateDurationFamilyBpm(const double seconds, const double sessionHintBpm) noexcept
{
    if (!(seconds > 1.0e-9) || !std::isfinite(seconds))
    {
        return (sessionHintBpm >= 30.0 && sessionHintBpm <= 320.0) ? sessionHintBpm : 120.0;
    }
    const double hint = (sessionHintBpm >= 30.0 && sessionHintBpm <= 320.0) ? sessionHintBpm : 120.0;
    double best = hint;
    double bestErr = 1.0e9;
    constexpr int kBarBeats = 4;
    constexpr int kBars[] = {1, 2, 4, 8, 16};
    for (int bars : kBars)
    {
        double bpm = 60.0 * static_cast<double>(kBarBeats * bars) / seconds;
        if (!std::isfinite(bpm) || bpm <= 0.0)
        {
            continue;
        }
        while (bpm > 180.0)
        {
            bpm *= 0.5;
        }
        while (bpm < 60.0 && bpm * 2.0 <= 240.0)
        {
            bpm *= 2.0;
        }
        const double err = std::abs(bpm - hint);
        if (err < bestErr)
        {
            bestErr = err;
            best = bpm;
        }
    }
    if (!(best >= 30.0) || best > 320.0)
    {
        return hint;
    }
    return best;
}
} // namespace retrolooper
