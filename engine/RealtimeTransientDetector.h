// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace retrolooper
{
/**
 * Energy-flux onset tracker for the empty-session first take.
 * process() is allocation-free; head/tail windows are reserved in the constructor.
 */
class RealtimeTransientDetector
{
  public:
    static constexpr int kMaxWindow = 8192;

    RealtimeTransientDetector();

    void prepare(double sampleRate) noexcept;
    void reset() noexcept;

    void processSample(float left, float right) noexcept;
    void process(const float* left, const float* right, int numSamples) noexcept;

    /** True when a wrap-window peak is strong enough to rotate the loop. */
    [[nodiscard]] bool suggestSeamRotate(std::size_t loopLengthSamples, std::size_t& rotateAmount) const noexcept;

    [[nodiscard]] std::uint64_t samplesProcessed() const noexcept { return samplesProcessed_; }

  private:
    [[nodiscard]] float absAtWrapOffset(std::int64_t off) const noexcept;

    int windowSamples_{1920};
    int headCount_{0};
    int tailWrite_{0};
    int tailCount_{0};
    std::uint64_t samplesProcessed_{0};
    std::unique_ptr<float[]> headAbs_{};
    std::unique_ptr<float[]> tailAbs_{};
};
} // namespace retrolooper
