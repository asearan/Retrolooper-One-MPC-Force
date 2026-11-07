// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <algorithm>
#include <cmath>

namespace retrolooper
{
/**
 * Constant-power stereo pan. pan −1 = full left, 0 = center (unity), +1 = full right.
 * Center stays at 1.0 / 1.0 so a centered strip is unchanged.
 */
inline void stereoPanGains(const float pan, float& gainL, float& gainR) noexcept
{
    const float p = std::clamp(pan, -1.0f, 1.0f);
    constexpr float kPiOver4 = 0.7853981633974483f;
    constexpr float kSqrt2 = 1.4142135623730951f;
    const float theta = (p + 1.0f) * kPiOver4;
    gainL = std::cos(theta) * kSqrt2;
    gainR = std::sin(theta) * kSqrt2;
}

[[nodiscard]] inline float panGainL(const float pan) noexcept
{
    float l = 1.0f;
    float r = 1.0f;
    stereoPanGains(pan, l, r);
    return l;
}

[[nodiscard]] inline float panGainR(const float pan) noexcept
{
    float l = 1.0f;
    float r = 1.0f;
    stereoPanGains(pan, l, r);
    return r;
}
} // namespace retrolooper
