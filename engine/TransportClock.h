// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <cstdint>

namespace retrolooper
{
/**
 * Transport timing. BPM and sample rate use relaxed atomics so the audio thread can read tempo
 * without torn loads when the UI/MIDI thread calls setTempo.
 */
class TransportClock
{
  public:
    void setSampleRate(double sampleRate) noexcept;
    void setTempo(double bpm) noexcept;
    void reset() noexcept;
    void setSamplePosition(std::int64_t position) noexcept { samplePosition_ = position; }
    void advance(int numSamples) noexcept;

    [[nodiscard]] std::int64_t samplePosition() const noexcept;
    [[nodiscard]] double sampleRate() const noexcept;
    [[nodiscard]] double tempoBpm() const noexcept;
    [[nodiscard]] double samplesPerBeat() const noexcept;

  private:
    std::atomic<double> sampleRate_{48000.0};
    std::atomic<double> bpm_{120.0};
    std::int64_t samplePosition_ = 0;
};
} // namespace retrolooper
