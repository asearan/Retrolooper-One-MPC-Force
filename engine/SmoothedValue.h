// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <algorithm>
#include <cmath>

namespace retrolooper
{
/**
 * Linear parameter smoother equivalent to juce::SmoothedValue (linear).
 * The looper engine stays JUCE-free on the audio thread; the ramp math matches
 * a 20 ms prepareToPlay() smoothing time.
 */
template <typename FloatType>
class SmoothedValue
{
  public:
    void reset(const double sampleRate, const double rampLengthSeconds) noexcept
    {
        const double sr = (sampleRate > 1.0 && std::isfinite(sampleRate)) ? sampleRate : 44100.0;
        const double sec = (rampLengthSeconds > 0.0 && std::isfinite(rampLengthSeconds)) ? rampLengthSeconds : 0.02;
        stepsToTarget_ = std::max(1, static_cast<int>(std::llround(sr * sec)));
        setCurrentAndTargetValue(current_);
    }

    void setCurrentAndTargetValue(const FloatType newValue) noexcept
    {
        current_ = newValue;
        target_ = newValue;
        step_ = static_cast<FloatType>(0);
        countdown_ = 0;
    }

    void setTargetValue(const FloatType newValue) noexcept
    {
        if (newValue == target_)
        {
            return;
        }
        target_ = newValue;
        countdown_ = stepsToTarget_;
        step_ = (target_ - current_) / static_cast<FloatType>(stepsToTarget_);
    }

    FloatType getNextValue() noexcept
    {
        if (countdown_ <= 0)
        {
            return target_;
        }
        --countdown_;
        current_ += step_;
        if (countdown_ == 0)
        {
            current_ = target_;
        }
        return current_;
    }

    void skip(const int numSamples) noexcept
    {
        const int n = std::max(0, numSamples);
        if (n >= countdown_)
        {
            setCurrentAndTargetValue(target_);
            return;
        }
        current_ += step_ * static_cast<FloatType>(n);
        countdown_ -= n;
    }

    /** Write the next block. A settled parameter is one fill, not a per-sample ramp. */
    void writeNext(FloatType* dest, const int numSamples) noexcept
    {
        const int n = std::max(0, numSamples);
        if (n == 0)
        {
            return;
        }
        if (dest == nullptr)
        {
            skip(n);
            return;
        }
        if (countdown_ <= 0)
        {
            std::fill_n(dest, static_cast<std::size_t>(n), target_);
            return;
        }
        const int ramp = std::min(n, countdown_);
        for (int i = 0; i < ramp; ++i)
        {
            dest[i] = getNextValue();
        }
        if (ramp < n)
        {
            std::fill_n(dest + ramp, static_cast<std::size_t>(n - ramp), target_);
        }
    }

    [[nodiscard]] FloatType getCurrentValue() const noexcept { return current_; }
    [[nodiscard]] FloatType getTargetValue() const noexcept { return target_; }
    [[nodiscard]] bool isSmoothing() const noexcept { return countdown_ > 0; }

  private:
    FloatType current_{};
    FloatType target_{};
    FloatType step_{static_cast<FloatType>(0)};
    int countdown_{0};
    int stepsToTarget_{1};
};
} // namespace retrolooper
