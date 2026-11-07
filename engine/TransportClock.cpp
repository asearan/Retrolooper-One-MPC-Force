// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#include "TransportClock.h"

namespace retrolooper
{
void TransportClock::setSampleRate(double sampleRate) noexcept
{
    sampleRate_.store(sampleRate > 0.0 ? sampleRate : 48000.0, std::memory_order_relaxed);
}

void TransportClock::setTempo(double bpm) noexcept
{
    bpm_.store(bpm > 1.0 ? bpm : 120.0, std::memory_order_relaxed);
}

void TransportClock::reset() noexcept
{
    samplePosition_ = 0;
}

void TransportClock::advance(int numSamples) noexcept
{
    samplePosition_ += numSamples;
}

std::int64_t TransportClock::samplePosition() const noexcept
{
    return samplePosition_;
}

double TransportClock::sampleRate() const noexcept
{
    return sampleRate_.load(std::memory_order_relaxed);
}

double TransportClock::tempoBpm() const noexcept
{
    return bpm_.load(std::memory_order_relaxed);
}

double TransportClock::samplesPerBeat() const noexcept
{
    const double sr = sampleRate_.load(std::memory_order_relaxed);
    const double b = bpm_.load(std::memory_order_relaxed);
    return sr * 60.0 / b;
}
} // namespace retrolooper
