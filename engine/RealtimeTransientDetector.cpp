// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#include "RealtimeTransientDetector.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace retrolooper
{
RealtimeTransientDetector::RealtimeTransientDetector()
    : headAbs_(std::make_unique<float[]>(static_cast<std::size_t>(kMaxWindow)))
    , tailAbs_(std::make_unique<float[]>(static_cast<std::size_t>(kMaxWindow)))
{
}

void RealtimeTransientDetector::prepare(const double sampleRate) noexcept
{
    const double sr = (sampleRate >= 8000.0) ? sampleRate : 48000.0;
    windowSamples_ = static_cast<int>(std::llround(sr * 0.040));
    windowSamples_ = std::clamp(windowSamples_, 8, kMaxWindow);
    reset();
}

void RealtimeTransientDetector::reset() noexcept
{
    headCount_ = 0;
    tailWrite_ = 0;
    tailCount_ = 0;
    samplesProcessed_ = 0;
}

void RealtimeTransientDetector::processSample(const float left, const float right) noexcept
{
    const float a = std::fabs(0.5f * (left + right));
    if (headCount_ < windowSamples_)
    {
        headAbs_[static_cast<std::size_t>(headCount_)] = a;
        ++headCount_;
    }
    tailAbs_[static_cast<std::size_t>(tailWrite_)] = a;
    ++tailWrite_;
    if (tailWrite_ >= windowSamples_)
    {
        tailWrite_ = 0;
    }
    if (tailCount_ < windowSamples_)
    {
        ++tailCount_;
    }
    ++samplesProcessed_;
}

void RealtimeTransientDetector::process(const float* left, const float* right, const int numSamples) noexcept
{
    if (left == nullptr || numSamples <= 0)
    {
        return;
    }
    const float* r = (right != nullptr) ? right : left;
    for (int i = 0; i < numSamples; ++i)
    {
        processSample(left[i], r[i]);
    }
}

float RealtimeTransientDetector::absAtWrapOffset(const std::int64_t off) const noexcept
{
    if (off >= 0)
    {
        if (off >= static_cast<std::int64_t>(headCount_))
        {
            return 0.0f;
        }
        return headAbs_[static_cast<std::size_t>(off)];
    }
    const auto fromEnd = static_cast<int>(-off);
    if (fromEnd <= 0 || fromEnd > tailCount_ || windowSamples_ <= 0)
    {
        return 0.0f;
    }
    int pos = tailWrite_ - fromEnd;
    if (pos < 0)
    {
        pos += windowSamples_;
    }
    return tailAbs_[static_cast<std::size_t>(pos)];
}

bool RealtimeTransientDetector::suggestSeamRotate(const std::size_t loopLengthSamples, std::size_t& rotateAmount) const noexcept
{
    rotateAmount = 0;
    const auto n = loopLengthSamples;
    if (n < 64 || windowSamples_ < 8 || samplesProcessed_ < 64)
    {
        return false;
    }
    const auto window = std::min(n / 4, static_cast<std::size_t>(windowSamples_));
    if (window < 8)
    {
        return false;
    }

    const auto wrap = [n](const std::int64_t i) noexcept -> std::size_t
    {
        const auto li = static_cast<std::int64_t>(n);
        auto r = i % li;
        if (r < 0)
        {
            r += li;
        }
        return static_cast<std::size_t>(r);
    };

    const auto w = static_cast<std::int64_t>(window);
    double sum = 0.0;
    double bestFlux = -1.0;
    std::int64_t bestOff = 0;
    int count = 0;
    float prev = absAtWrapOffset(-w - 1);
    for (std::int64_t off = -w; off <= w; ++off)
    {
        const float a = absAtWrapOffset(off);
        const double flux = std::max(0.0, static_cast<double>(a) - static_cast<double>(prev));
        prev = a;
        sum += flux;
        ++count;
        if (flux > bestFlux)
        {
            bestFlux = flux;
            bestOff = off;
        }
    }
    const double mean = sum / static_cast<double>(std::max(1, count));
    if (bestOff == 0 || bestFlux < 0.02 || bestFlux < 3.0 * (mean + 1.0e-12))
    {
        return false;
    }
    rotateAmount = wrap(bestOff);
    return rotateAmount > 0 && rotateAmount < n;
}
} // namespace retrolooper
